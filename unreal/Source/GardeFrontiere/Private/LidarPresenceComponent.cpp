#include "LidarPresenceComponent.h"

#include "GardeFrontiere.h"
#include "SerialCom.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Async/Async.h"

ULidarPresenceComponent::ULidarPresenceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// Sans auto-activation, IsActive() est faux au BeginPlay — qui s'execute
	// AVANT le SetActive() du gestionnaire, dispatche par Super::BeginPlay()
	// de l'acteur. Le port ne s'ouvrait donc jamais, sauf a cocher la case a
	// la main dans l'editeur.
	bAutoActivate = true;
}

void ULidarPresenceComponent::Activate(bool bReset)
{
	Super::Activate(bReset);

	// Reactivation apres coup (interrupteur de diagnostic rendu au
	// gestionnaire, ou pilotage Blueprint) : on rouvre ce que Deactivate a
	// ferme. Au demarrage, HasBegunPlay() est encore faux et c'est BeginPlay
	// qui ouvrira.
	if (HasBegunPlay() && !bPortOuvert)
	{
		TentativesEchouees = 0;
		OuvrirPort();
	}
}

void ULidarPresenceComponent::Deactivate()
{
	Super::Deactivate();

	// L'interrupteur doit couper VRAIMENT : minuteries de lecture et de
	// reconnexion comprises. Avant, un SetActive(false) arrivant apres le
	// BeginPlay laissait le port ouvert et la lecture tourner — le
	// sous-systeme cense etre isolable ne l'etait pas.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearAllTimersForObject(this);
	}
	FermerPort();

	// Un capteur qu'on coupe ne doit pas laisser un visiteur bloque en
	// session — meme contrat qu'une liaison perdue.
	if (bPresent)
	{
		bPresent = false;
		OnPresencePerdue.Broadcast();
	}
}

void ULidarPresenceComponent::BeginPlay()
{
	Super::BeginPlay();

	// Le gestionnaire peut desactiver le composant a des fins de
	// diagnostic : on n'ouvre alors aucun port.
	if (IsActive())
	{
		OuvrirPort();
	}
	else
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Capteur : composant inactif, port non ouvert"));
	}
}

void ULidarPresenceComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	// Les minuteries d'abord : une lecture ou une tentative d'ouverture qui
	// se declencherait pendant la fermeture bloquerait le thread de jeu.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearAllTimersForObject(this);
	}
	FermerPort();
	Super::EndPlay(Raison);
}

// -- Liaison serie -------------------------------------------------------

void ULidarPresenceComponent::ProgrammerReconnexion()
{
	UWorld* Monde = GetWorld();
	if (!Monde)
	{
		return;
	}

	// Espacement croissant, plafonne a 30 s. L'ancienne version reessayait
	// toutes les 5 s indefiniment et noyait le journal — au point de rendre
	// illisibles les messages qui comptent.
	++TentativesEchouees;
	const float Delai = FMath::Min(DelaiReconnexion * TentativesEchouees, 30.f);

	// On ne journalise que les premieres tentatives, puis une sur dix.
	if (TentativesEchouees <= 3 || TentativesEchouees % 10 == 0)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Capteur : COM%d indisponible (tentative %d) — nouvel essai dans %.0f s"),
			PortCOM, TentativesEchouees, Delai);
	}

	Monde->GetTimerManager().SetTimer(
		MinuterieReconnexion, this, &ULidarPresenceComponent::OuvrirPort, Delai, false);
}

void ULidarPresenceComponent::OuvrirPort()
{
	UWorld* Monde = GetWorld();

	// Ne rien tenter pendant la destruction : l'ouverture d'un port serie
	// est un appel systeme bloquant, et le declencher au moment ou le
	// monde disparait fige le thread de jeu. Ni desactive : une minuterie
	// de reconnexion qui survivrait a Deactivate ne doit pas rouvrir.
	if (!Monde || IsBeingDestroyed() || !IsValid(this) || !IsActive())
	{
		return;
	}

	bool bOuvert = false;
	Serie = USerialCom::OpenComPort(bOuvert, PortCOM, VitesseBauds);
	bPortOuvert = bOuvert && Serie != nullptr;

	if (bPortOuvert)
	{
		TentativesEchouees = 0;
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Capteur : COM%d ouvert a %d bauds"), PortCOM, VitesseBauds);

		Monde->GetTimerManager().SetTimer(
			MinuterieLecture, this, &ULidarPresenceComponent::Lire,
			FMath::Max(PeriodeLecture, 0.02f), true);
	}
	else
	{
		// L'ancien projet abandonnait ici : le capteur debranche condamnait
		// la borne jusqu'au redemarrage. On reessaie, en espacant.
		Serie = nullptr;
		ProgrammerReconnexion();
	}
}

void ULidarPresenceComponent::FermerPort()
{
	if (Serie)
	{
		Serie->Close();
		Serie = nullptr;
	}
	bPortOuvert = false;
}

// -- Lecture -------------------------------------------------------------

void ULidarPresenceComponent::Lire()
{
	// Une seule lecture en vol a la fois : USerialCom lit octet par octet
	// et peut attendre jusqu'a 2 secondes par octet. En empiler plusieurs
	// saturerait le pool de threads.
	//
	// Teste AVANT le diagnostic de liaison : fermer le port pendant qu'une
	// lecture court encore sur le thread de fond serait un acces a un
	// handle clos. Si la liaison est vraiment morte, on la constatera au
	// cycle suivant, la lecture en vol rendue.
	if (bLectureEnVol.Load())
	{
		return;
	}

	if (!Serie || !Serie->IsOpened())
	{
		UE_LOG(LogGardeFrontiere, Warning, TEXT("Capteur : liaison perdue"));

		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().ClearTimer(MinuterieLecture);
		}
		FermerPort();

		// Un capteur muet ne doit pas laisser un visiteur bloque en session.
		if (bPresent)
		{
			bPresent = false;
			OnPresencePerdue.Broadcast();
		}
		ProgrammerReconnexion();
		return;
	}

	bLectureEnVol.Store(true);

	TWeakObjectPtr<ULidarPresenceComponent> Faible(this);
	USerialCom* Port = Serie;

	// Le port reste ancre tant que la lecture est en vol. FermerPort (arret,
	// liaison perdue) lache la seule reference UPROPERTY : sans cet ancrage,
	// le GC pouvait detruire l'objet PENDANT que le thread de fond etait
	// dans Readln — lecture sur memoire liberee, crash ou gel. AddToRoot et
	// RemoveFromRoot s'executent tous deux sur le thread de jeu.
	Port->AddToRoot();

	// Le WaitForSingleObject de 2 s du plugin figerait boutons et menus s'il
	// s'executait ici. On le deporte, et on ne revient sur le thread de jeu
	// que pour appliquer le resultat.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[Faible, Port]()
		{
			FString Derniere;
			int32 NbLues = 0;

			if (IsValid(Port))
			{
				// On VIDE le tampon a chaque cycle, au lieu de n'en retirer
				// qu'une ligne.
				//
				// L'Arduino emet a ~90 Hz et on lit a 10 Hz : en ne consommant
				// qu'une ligne par cycle, huit sur neuf s'entassaient dans le
				// tampon systeme. On lisait donc des mesures de plus en plus
				// vieilles, jusqu'a saturation et perte — un visiteur aurait
				// ete detecte avec plusieurs secondes de retard, ou pas du tout.
				//
				// ReadStringUntil teste ComStat.cbInQue en entree et rend la
				// main aussitot si rien n'attend : la boucle s'arrete d'elle-
				// meme. Le plafond n'est qu'un garde-fou contre un capteur
				// devenu fou.
				bool bCoup = false;
				FString Ligne = Port->Readln(bCoup);

				while (bCoup && NbLues < 500)
				{
					++NbLues;
					if (!Ligne.IsEmpty())
					{
						Derniere = Ligne;
					}
					bCoup = false;
					Ligne = Port->Readln(bCoup);
				}
			}

			AsyncTask(ENamedThreads::GameThread,
				[Faible, Port, Derniere, NbLues]()
				{
					// La lecture est rendue : le port peut repartir au GC.
					// Inconditionnel — meme si le composant a disparu.
					Port->RemoveFromRoot();

					ULidarPresenceComponent* Comp = Faible.Get();
					if (!Comp)
					{
						return;   // le composant a disparu entre-temps
					}
					Comp->bLectureEnVol.Store(false);

					// Le port a pu etre ferme et rouvert pendant la lecture :
					// un releve d'une liaison morte n'a plus de valeur.
					if (Comp->Serie == Port && !Derniere.IsEmpty())
					{
						Comp->MesuresParCycle = NbLues;
						Comp->TraiterReleve(Derniere);
					}
				});
		});
}

void ULidarPresenceComponent::TraiterReleve(const FString& Brut)
{
	const FString Ligne = Brut.TrimStartAndEnd();
	if (Ligne.IsEmpty() || !Ligne.IsNumeric())
	{
		// Ecarter en silence etait le comportement d'origine, et c'est ce qui
		// a rendu une vitesse de liaison erronee indetectable : le port
		// s'ouvrait, aucune trame n'etait lisible, et rien ne le disait.
		//
		// On ecarte toujours — une trame partielle est normale — mais un flot
		// continu de rejets ne l'est pas, et se signale une fois.
		++CompteurRejets;
		if (!Ligne.IsEmpty())
		{
			DernierRejet = Ligne.Left(40);
		}

		if (CompteurRejets >= 50 && !bRejetSignale)
		{
			bRejetSignale = true;
			UE_LOG(LogGardeFrontiere, Warning,
				TEXT("Capteur : %d trames illisibles d'affilee sur COM%d — vitesse ")
				TEXT("probablement incorrecte (%d bauds configures). Dernier echantillon : '%s'"),
				CompteurRejets, PortCOM, VitesseBauds, *DernierRejet);
		}

		// A l'ecran aussi, sinon l'absence de mesure serait muette : un
		// capteur illisible n'affiche rien, exactement comme un capteur
		// deconnecte ou un composant desactive.
		if (bTracerReleves && CompteurRejets >= 50 && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(CleAffichageLiaison, 2.f, FColor::Red,
				FString::Printf(TEXT("LiDAR  COM%d a %d bauds : %d trames illisibles — verifier la vitesse"),
					PortCOM, VitesseBauds, CompteurRejets));
		}
		return;
	}

	const int32 Distance = FCString::Atoi(*Ligne);
	if (Distance <= 0)
	{
		return;
	}

	// Une trame valide remet le compteur a zero — et rearme l'avertissement,
	// pour qu'une liaison qui se degrade en cours de route se signale a
	// nouveau plutot que de rester muette apres un seul message.
	CompteurRejets = 0;
	bRejetSignale = false;

	DerniereDistanceCm = Distance;

	// -- Capteur fige ----------------------------------------------------
	//
	// Comparaison stricte, volontairement. Un telemetre a temps de vol
	// bruite toujours d'un centimetre ou deux, meme fixe face a un mur :
	// une egalite parfaite repetee des centaines de fois ne peut pas etre
	// une mesure. Une tolerance rendrait le test aveugle a ce qu'il cherche.
	if (Distance == ValeurRepetee)
	{
		++CompteurIdentiques;

		if (MesuresIdentiquesAvantAlerte > 0
			&& CompteurIdentiques >= MesuresIdentiquesAvantAlerte
			&& !bCapteurFige)
		{
			bCapteurFige = true;
			UE_LOG(LogGardeFrontiere, Error,
				TEXT("Capteur : %d mesures identiques a %d cm — capteur probablement ")
				TEXT("fige. Reinitialiser l'Arduino, puis verifier le cablage du module."),
				CompteurIdentiques, Distance);
		}
	}
	else
	{
		ValeurRepetee = Distance;
		CompteurIdentiques = 1;

		// Le capteur remesure : on rearme, pour qu'un second blocage se
		// signale aussi. Une alerte qui ne se leve qu'une fois par session
		// ne vaut rien sur une borne qui tourne des journees entieres.
		if (bCapteurFige)
		{
			bCapteurFige = false;
			UE_LOG(LogGardeFrontiere, Log, TEXT("Capteur : mesures reparties (%d cm)"), Distance);
		}
	}

	if (bCapteurFige && bTracerReleves && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(CleAffichageFige, 2.f, FColor::Orange,
			FString::Printf(TEXT("LiDAR  FIGE a %d cm depuis %d mesures — reinitialiser l'Arduino"),
				Distance, CompteurIdentiques));
	}

	if (bTracerReleves)
	{
		// A l'ECRAN, a chaque mesure. Le journal ne se lit qu'apres coup :
		// debout devant le capteur, on ne peut pas etre a la fois le visiteur
		// et le lecteur du fichier. Sans ce retour immediat, un capteur qui
		// ne voit rien et une suite de traitement bloquee sont indiscernables.
		//
		// La cle fixe fait que le message se REMPLACE au lieu de s'empiler.
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(CleAffichageCapteur, 1.5f,
				bPresent ? FColor::Green : FColor::Silver,
				FString::Printf(TEXT("LiDAR  %4d cm    entree %d / sortie %d    %s  [%d/%d]"),
					Distance, SeuilPresenceCm, SeuilPresenceCm + HysteresisCm,
					bPresent ? TEXT("PRESENT") : TEXT("personne"),
					bPresent ? CompteurLoin : CompteurProche,
					bPresent ? ReleveseAvantAbsence : ReleveseAvantPresence));
		}

		// Dans le JOURNAL, une ligne par seconde au plus : a 10 Hz, tout
		// tracer noierait le fichier sans rien apprendre de plus.
		const double Maintenant = FPlatformTime::Seconds();
		if (Maintenant - DerniereTrace >= 1.0)
		{
			DerniereTrace = Maintenant;
			UE_LOG(LogGardeFrontiere, Log,
				TEXT("Capteur : %d cm (seuil %d, sortie %d) — %s [%d mesures/cycle]"),
				Distance, SeuilPresenceCm, SeuilPresenceCm + HysteresisCm,
				bPresent ? TEXT("present") : TEXT("personne"), MesuresParCycle);
		}
	}

	// Hysteresis : le seuil de sortie est plus large que celui d'entree,
	// pour qu'un visiteur pile a la limite ne fasse pas osciller la borne.
	const int32 SeuilSortie = SeuilPresenceCm + HysteresisCm;

	if (Distance <= SeuilPresenceCm)
	{
		CompteurLoin = 0;
		++CompteurProche;

		if (!bPresent && CompteurProche >= ReleveseAvantPresence)
		{
			bPresent = true;
			UE_LOG(LogGardeFrontiere, Log, TEXT("Capteur : presence a %d cm"), Distance);
			OnPresenceDetectee.Broadcast();
		}
	}
	else if (Distance > SeuilSortie)
	{
		CompteurProche = 0;
		++CompteurLoin;

		if (bPresent && CompteurLoin >= ReleveseAvantAbsence)
		{
			bPresent = false;
			UE_LOG(LogGardeFrontiere, Log, TEXT("Capteur : zone liberee (%d cm)"), Distance);
			OnPresencePerdue.Broadcast();
		}
	}
	// Entre les deux seuils : zone morte, on ne change rien.
}

void ULidarPresenceComponent::ForcerPresence(bool bNouvelEtat)
{
	if (bNouvelEtat == bPresent)
	{
		return;
	}

	bPresent = bNouvelEtat;
	CompteurProche = CompteurLoin = 0;

	UE_LOG(LogGardeFrontiere, Log, TEXT("Capteur : presence forcee a %s"),
		bNouvelEtat ? TEXT("vrai") : TEXT("faux"));

	if (bNouvelEtat) { OnPresenceDetectee.Broadcast(); }
	else             { OnPresencePerdue.Broadcast(); }
}
