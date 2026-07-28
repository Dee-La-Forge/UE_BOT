#include "LidarPresenceComponent.h"

#include "GardeFrontiere.h"
#include "SerialCom.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Async/Async.h"

ULidarPresenceComponent::ULidarPresenceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
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
	// monde disparait fige le thread de jeu.
	if (!Monde || IsBeingDestroyed() || !IsValid(this))
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

	// Une seule lecture en vol a la fois : USerialCom lit octet par octet
	// et peut attendre jusqu'a 2 secondes par octet. En empiler plusieurs
	// saturerait le pool de threads.
	if (bLectureEnVol.Load())
	{
		return;
	}
	bLectureEnVol.Store(true);

	TWeakObjectPtr<ULidarPresenceComponent> Faible(this);
	USerialCom* Port = Serie;

	// Le WaitForSingleObject de 2 s du plugin figerait boutons et menus s'il
	// s'executait ici. On le deporte, et on ne revient sur le thread de jeu
	// que pour appliquer le resultat.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[Faible, Port]()
		{
			bool bSucces = false;
			FString Ligne;

			if (IsValid(Port))
			{
				Ligne = Port->Readln(bSucces);
			}

			AsyncTask(ENamedThreads::GameThread,
				[Faible, Ligne, bSucces]()
				{
					ULidarPresenceComponent* Comp = Faible.Get();
					if (!Comp)
					{
						return;   // le composant a disparu entre-temps
					}
					Comp->bLectureEnVol.Store(false);

					if (bSucces && !Ligne.IsEmpty())
					{
						Comp->TraiterReleve(Ligne);
					}
				});
		});
}

void ULidarPresenceComponent::TraiterReleve(const FString& Brut)
{
	const FString Ligne = Brut.TrimStartAndEnd();
	if (Ligne.IsEmpty() || !Ligne.IsNumeric())
	{
		return;   // trame partielle ou bruit : on ignore, sans compter
	}

	const int32 Distance = FCString::Atoi(*Ligne);
	if (Distance <= 0)
	{
		return;
	}
	DerniereDistanceCm = Distance;

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
