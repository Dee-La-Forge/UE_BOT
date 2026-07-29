#include "GuardSessionManager.h"

#include "GardeFrontiere.h"
#include "SidecarClient.h"
#include "LidarPresenceComponent.h"
#include "AgentVoiceComponent.h"
#include "AgentFaceComponent.h"
#include "GlitchComponent.h"
#include "AvatarSwitcherComponent.h"
#include "StampComponent.h"
#include "VisitorMicComponent.h"
#include "AudioBridge.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

AGuardSessionManager::AGuardSessionManager()
{
	PrimaryActorTick.bCanEverTick = false;

	Presence = CreateDefaultSubobject<ULidarPresenceComponent>(TEXT("Presence"));

	Voix = CreateDefaultSubobject<UAgentVoiceComponent>(TEXT("Voix"));
	Voix->SetupAttachment(RootComponent);

	Visage  = CreateDefaultSubobject<UAgentFaceComponent>(TEXT("Visage"));
	Glitch  = CreateDefaultSubobject<UGlitchComponent>(TEXT("Glitch"));
	Avatars = CreateDefaultSubobject<UAvatarSwitcherComponent>(TEXT("Avatars"));
	Tampons = CreateDefaultSubobject<UStampComponent>(TEXT("Tampons"));
	Micro   = CreateDefaultSubobject<UVisitorMicComponent>(TEXT("Micro"));
}

void AGuardSessionManager::BeginPlay()
{
	Super::BeginPlay();

	Sidecar = NewObject<USidecarClient>(this);
	Sidecar->OnParoleDebut.AddDynamic(this, &AGuardSessionManager::SurParoleDebut);
	Sidecar->OnParoleFin.AddDynamic(this, &AGuardSessionManager::SurParoleFin);
	Sidecar->OnVerdict.AddDynamic(this, &AGuardSessionManager::SurVerdict);
	Sidecar->OnSessionTerminee.AddDynamic(this, &AGuardSessionManager::SurSessionTerminee);
	Sidecar->OnPanne.AddDynamic(this, &AGuardSessionManager::SurPanneIA);

	// Les trames audio arrivent en binaire, hors du systeme de delegues
	// Blueprint : trop volumineuses pour y transiter. On les route
	// directement vers la voix.
	Sidecar->OnAudioRecu.AddLambda(
		[this](const TArray<uint8>& PCM16, int32 Taux)
		{
			if (Voix)
			{
				Voix->EmpilerTrame(PCM16, Taux);
			}
		});

	if (bActiverSidecar)
	{
		Sidecar->Connecter(UrlSidecar);
	}
	else
	{
		UE_LOG(LogGardeFrontiere, Warning, TEXT("DIAGNOSTIC : sidecar desactive"));
	}

	if (Presence)
	{
		Presence->OnPresenceDetectee.AddDynamic(this, &AGuardSessionManager::SurPresenceDetectee);
		Presence->OnPresencePerdue.AddDynamic(this, &AGuardSessionManager::SurPresencePerdue);
		Presence->SetActive(bActiverCapteur);

		if (!bActiverCapteur)
		{
			UE_LOG(LogGardeFrontiere, Warning, TEXT("DIAGNOSTIC : capteur desactive"));
		}
	}

	// Le glitch masque la substitution : on ne permute qu'une fois l'effet
	// installe, sinon le visiteur verrait l'avatar disparaitre puis
	// reapparaitre a l'ecran.
	if (Glitch)
	{
		Glitch->OnGlitchTermine.AddDynamic(this, &AGuardSessionManager::SurGlitchTermine);
	}

	// La parole du visiteur, une fois bornee par le VAD, part au sidecar par
	// le meme chemin que si un Blueprint l'avait transmise — c'est
	// TransmettreParoleVisiteur qui reste le point d'entree unique, avec son
	// controle de niveau et sa conversion.
	if (Micro)
	{
		Micro->OnSegmentVisiteur.AddLambda(
			[this](const TArray<float>& Echantillons, int32 Taux, int32 NbCanaux)
			{
				TransmettreParoleVisiteur(Echantillons, Taux, NbCanaux);
			});
	}

	// A chaque nouvel avatar, le composant d'expression doit recibler le
	// maillage facial : l'ancien vient d'etre detruit.
	if (Avatars)
	{
		Avatars->OnAvatarChange.AddDynamic(this, &AGuardSessionManager::SurAvatarChange);
	}

	ChangerPhase(EGuardPhase::Veille);

	// Un garde tient le poste avant meme le premier visiteur. Le spawn passe
	// par Permuter() plutot que par un Spawner(0) fixe : l'agent de veille est
	// ainsi tire au sort comme les autres, et surtout il devient l'avatar
	// precedent — le premier visiteur en verra donc necessairement un autre.
	//
	// Place apres le branchement de OnAvatarChange, sans quoi le composant
	// d'expression ne ciblerait pas le maillage facial de ce premier avatar.
	if (bAvatarEnVeille && Avatars)
	{
		Avatars->Permuter();
	}

	UE_LOG(LogGardeFrontiere, Log, TEXT("Borne prete — en veille"));
}

void AGuardSessionManager::EndPlay(const EEndPlayReason::Type Raison)
{
	// Instrumentation : le gel survient a l'arret du PIE et resiste a trois
	// correctifs. Ces traces designent l'etape exacte ou le thread se bloque
	// — la derniere ligne ecrite est celle qui precede le blocage.
	UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET 1/6 : debut EndPlay"));

	// Ordre important : on coupe les sources d'evenements avant de detruire
	// quoi que ce soit. Un evenement arrivant en pleine destruction fige le
	// thread de jeu — ou pire, le fait planter.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearAllTimersForObject(this);
	}
	UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET 2/6 : minuteries annulees"));

	if (Sidecar)
	{
		// Le lambda branche sur OnAudioRecu capture `this` : le detacher
		// avant la destruction evite qu'une trame tardive n'y accede.
		Sidecar->OnAudioRecu.Clear();
		UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET 3/6 : delegue audio detache"));

		Sidecar->Deconnecter();
		Sidecar = nullptr;
	}
	UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET 4/6 : sidecar deconnecte"));

	if (Voix) { Voix->Interrompre(); }
	UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET 5/6 : voix interrompue"));

	if (Glitch) { Glitch->Arreter(); }
	UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET 6/6 : glitch arrete"));

	Super::EndPlay(Raison);
	UE_LOG(LogGardeFrontiere, Warning, TEXT("ARRET : termine proprement"));
}

// -- Machine a etats -----------------------------------------------------

void AGuardSessionManager::ChangerPhase(EGuardPhase Nouvelle)
{
	if (Phase == Nouvelle)
	{
		return;
	}

	const UEnum* Enum = StaticEnum<EGuardPhase>();
	UE_LOG(LogGardeFrontiere, Log, TEXT("Phase : %s -> %s"),
		*Enum->GetDisplayNameTextByValue((int64)Phase).ToString(),
		*Enum->GetDisplayNameTextByValue((int64)Nouvelle).ToString());

	// A l'ecran : savoir que le capteur a vu quelqu'un ne suffit pas, encore
	// faut-il voir si la machine a etats a suivi. Sans cet affichage, une
	// detection qui n'aboutit pas et une detection qui n'a pas eu lieu se
	// ressemblent trait pour trait.
	if (bAfficherEtatEcran && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(CleAffichagePhase, 6.f, FColor::Cyan,
			FString::Printf(TEXT("Phase  %s"),
				*Enum->GetDisplayNameTextByValue((int64)Nouvelle).ToString()));
	}

	Phase = Nouvelle;
	OnPhaseChangee.Broadcast(Nouvelle);

	// L'ecoute suit la session, pas le detail des phases : l'agent fait face
	// au visiteur du moment ou il se presente jusqu'a ce qu'il parte. Une
	// posture qui changerait a chaque phase donnerait un personnage agite.
	const bool bEcouteVoulue = (Nouvelle != EGuardPhase::Veille);
	if (bEcouteVoulue != bEnEcoute)
	{
		bEnEcoute = bEcouteVoulue;
		AppliquerPosture();

		// Le micro suit la session. La capture, elle, reste ouverte en
		// permanence : ouvrir un peripherique audio prend des centaines de
		// millisecondes, et les payer a l'arrivee du visiteur lui couterait
		// ses premiers mots.
		if (Micro)
		{
			bEnEcoute ? Micro->DemarrerEcoute() : Micro->ArreterEcoute();
		}

		OnEcouteChangee.Broadcast(bEnEcoute);
	}
}

void AGuardSessionManager::AppliquerPosture()
{
	if (!Avatars)
	{
		return;
	}

	UAnimationAsset* Voulue = bEnEcoute ? AnimationEcoute.Get() : AnimationVeille.Get();

	// Une case laissee vide rend la main a l'AnimBP du MetaHuman plutot que
	// de figer l'agent sur la derniere pose jouee.
	Avatars->JouerAnimationCorps(Voulue);
}

void AGuardSessionManager::DemarrerSession()
{
	if (Phase != EGuardPhase::Veille)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Demarrage demande alors qu'une session est en cours — ignore"));
		return;
	}

	DernierVerdict = EGuardVerdict::EnCours;

	// L'index reel est renseigne par SurAvatarChange, une fois la
	// permutation faite par UAvatarSwitcherComponent.
	ChangerPhase(EGuardPhase::Accueil);

	if (Tampons)
	{
		Tampons->Masquer();
	}

	// Aucun glitch a l'arrivee : la substitution a eu lieu au depart du
	// visiteur precedent, l'agent en poste est deja le bon. Un visiteur qui
	// se presente doit trouver quelqu'un, pas un ecran brouille.
	//
	// Reste le cas ou il arrive AVANT que la substitution precedente ne soit
	// finie — deux visiteurs qui se suivent de pres. On la termine seance
	// tenante plutot que de l'abandonner : sans quoi il aurait le meme visage
	// que son predecesseur, ce que toute la mecanique cherche a eviter.
	if (Glitch && Glitch->EstEnCours())
	{
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Visiteur pendant une substitution — on l'acheve immediatement"));
		Glitch->Arreter();
		if (Avatars)
		{
			Avatars->Permuter();
		}
	}

	OuvrirLaScene();
	ArmerAbandon();
}

void AGuardSessionManager::TransmettreParoleVisiteur(
	const TArray<float>& Echantillons, int32 TauxSource, int32 NbCanaux)
{
	if (Phase == EGuardPhase::Veille || Echantillons.Num() == 0)
	{
		return;
	}

	// Un micro debranche ou coupe produit un segment silencieux. L'envoyer
	// ferait transcrire du vide et repondre l'agent dans le vent.
	const float Crete = UAudioBridge::NiveauCrete(Echantillons);
	if (Crete < 0.005f)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Parole visiteur ignoree : niveau quasi nul (%.4f) — micro absent ou coupe"),
			Crete);
		return;
	}

	const TArray<uint8> PCM16 =
		UAudioBridge::VersPCM16Visiteur(Echantillons, TauxSource, NbCanaux);

	if (Sidecar && Sidecar->EstConnecte())
	{
		// Le visiteur repond pour la premiere fois : l'accueil est termine,
		// l'interrogatoire commence. C'est le meme decoupage que le sidecar,
		// dont la phase INTRO se ferme sur la premiere reponse.
		if (Phase == EGuardPhase::Accueil)
		{
			ChangerPhase(EGuardPhase::Interrogatoire);
		}

		Sidecar->EnvoyerAudioVisiteur(PCM16);
		ArmerAbandon();   // le visiteur parle : on repousse l'abandon
	}
	else
	{
		OnRepliqueDeSecours.Broadcast(TEXT("Repetez."));
	}
}

void AGuardSessionManager::TerminerSession(EGuardFinDeSession Raison)
{
	if (Phase == EGuardPhase::Veille)
	{
		return;
	}

	AnnulerMinuteries();

	// On ne laisse ni l'agent parler dans le vide, ni un tampon ou un
	// effet rester affiche apres le depart du visiteur.
	if (Voix)    { Voix->Interrompre(); }
	if (Tampons) { Tampons->Masquer(); }
	if (Glitch)  { Glitch->Arreter(); }
	if (Visage)  { Visage->Reinitialiser(); }

	const UEnum* Enum = StaticEnum<EGuardFinDeSession>();
	UE_LOG(LogGardeFrontiere, Log, TEXT("Fin de session : %s"),
		*Enum->GetDisplayNameTextByValue((int64)Raison).ToString());

	if (Sidecar && Sidecar->EstConnecte())
	{
		Sidecar->ReinitialiserSession();
	}

	ChangerPhase(EGuardPhase::Veille);
	OnSessionFinie.Broadcast(Raison);

	// La zone se libere : c'est MAINTENANT qu'on change de visage, derriere
	// le glitch, sans temoin. Le visiteur suivant trouvera un autre agent
	// sans avoir vu la substitution — c'est tout l'objet du dispositif.
	//
	// Declenche apres le Arreter() du nettoyage ci-dessus, sinon on eteindrait
	// l'effet qu'on vient d'allumer.
	if (Glitch)
	{
		Glitch->Declencher();   // la permutation suivra OnGlitchTermine
	}
	else if (Avatars)
	{
		Avatars->Permuter();
	}
}

// -- Capteur de presence -------------------------------------------------

void AGuardSessionManager::SurPresenceDetectee()
{
	if (Phase == EGuardPhase::Veille)
	{
		DemarrerSession();
	}
}

void AGuardSessionManager::SurPresencePerdue()
{
	if (Phase == EGuardPhase::Veille)
	{
		return;
	}

	// Depart apres le verdict : deroulement nominal, la place est liberee.
	// Depart avant : abandon.
	const bool bApresVerdict =
		Phase == EGuardPhase::SortieZone || Phase == EGuardPhase::Verdict;

	// Le sidecar est prevenu par TerminerSession, et par elle seule. On
	// envoyait ici presence.perdue puis, dans la foulee, session.reset — deux
	// messages pour un seul evenement, que le sidecar traite a l'identique.
	// Son journal en portait la trace en double a chaque depart, ce qui
	// masquera un vrai probleme le jour ou il s'en presentera un.

	TerminerSession(bApresVerdict
		? EGuardFinDeSession::Nominale
		: EGuardFinDeSession::Abandon);
}

// -- Sidecar -------------------------------------------------------------

void AGuardSessionManager::SurGlitchTermine()
{
	// Le glitch ne joue plus qu'entre deux visiteurs, la zone vide. Il n'y a
	// donc plus rien a garder ici : permuter est tout ce qu'il reste a faire,
	// et le prochain visiteur trouvera le nouveau visage en poste.
	//
	// Si un visiteur s'est presente entre-temps, DemarrerSession a deja
	// arrete l'effet et permute : cette fonction n'est alors pas appelee.
	if (Avatars)
	{
		Avatars->Permuter();
	}
}

void AGuardSessionManager::OuvrirLaScene()
{
	// L'index n'est fiable qu'ICI. Diffuse au demarrage de la session, il
	// designait encore l'avatar precedent : la substitution n'a lieu qu'a la
	// fin du glitch. Invisible avec un seul avatar renseigne, faux des qu'il
	// y en a plusieurs — et la scenographie se serait trompee de personnage.
	OnSessionDemarree.Broadcast(IndexAvatarCourant);

	if (Sidecar && Sidecar->EstConnecte())
	{
		Sidecar->SignalerPresence();
	}
	else
	{
		// Mode degrade : la borne accueille quand meme.
		OnRepliqueDeSecours.Broadcast(TEXT("Papiers. Garde-frontiere."));
	}
}

void AGuardSessionManager::SurAvatarChange(AActor* NouvelAvatar, int32 Index)
{
	IndexAvatarCourant = Index;

	// L'ancien maillage vient d'etre detruit : sans ce reciblage, les
	// ecritures d'emotion partiraient dans le vide, sans erreur visible.
	if (Visage && Avatars)
	{
		Visage->CiblerMaillage(Avatars->TrouverMaillageFacial());
	}

	// Le nouvel avatar naît dans la pose de son AnimBP : il faut lui rendre
	// la posture de la session en cours, sans quoi la substitution ferait
	// repasser l'agent en position de garde devant un visiteur present.
	AppliquerPosture();
}

void AGuardSessionManager::SurParoleDebut(const FString& Texte, EGuardEmotion Emotion)
{
	bIADisponible = true;

	// La phase ne bascule PLUS ici. La premiere replique de l'agent est son
	// accueil : cote sidecar c'est la phase INTRO, qui ne compte pas comme
	// une question. Basculer des qu'il ouvre la bouche faisait diverger les
	// deux machines a etats des le premier mot — sans consequence visible
	// aujourd'hui, mais c'est l'ecart qui fabrique un bug le jour ou une
	// regle s'appuie dessus.
	//
	// L'interrogatoire commence quand le visiteur repond : voir
	// TransmettreParoleVisiteur.

	if (Visage)
	{
		Visage->AppliquerEmotion(Emotion);
	}
	OnEmotionChangee.Broadcast(Emotion);

	// Tant que l'echange vit, l'abandon est repousse.
	ArmerAbandon();
}

void AGuardSessionManager::SurParoleFin()
{
	ArmerAbandon();
}

void AGuardSessionManager::SurVerdict(EGuardVerdict Decision)
{
	DernierVerdict = Decision;
	ChangerPhase(EGuardPhase::Verdict);

	if (Tampons)
	{
		Tampons->AfficherVerdict(Decision);
	}
	OnVerdictRendu.Broadcast(Decision);
}

void AGuardSessionManager::SurSessionTerminee()
{
	// On laisse le visiteur lire son tampon avant de le prier de sortir.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().SetTimer(
			MinuterieSortie, this, &AGuardSessionManager::SurDelaiSortie,
			FMath::Max(DelaiAvantSortie, 0.01f), false);
	}
}

void AGuardSessionManager::SurDelaiSortie()
{
	ChangerPhase(EGuardPhase::SortieZone);

	if (Tampons)
	{
		Tampons->AfficherSortieZone();
	}
	OnDemandeSortieZone.Broadcast();

	// A partir d'ici, seul le depart du visiteur clot la session. On garde
	// toutefois un abandon arme : sans lui, un visiteur qui reste plante
	// devant la borne la bloquerait indefiniment.
	ArmerAbandon();
}

void AGuardSessionManager::SurPanneIA(const FString& Raison)
{
	bIADisponible = false;
	UE_LOG(LogGardeFrontiere, Error, TEXT("IA indisponible : %s"), *Raison);

	// Une borne muette avec un visiteur planté devant est le seul echec
	// vraiment couteux. On parle, meme mal.
	if (Phase != EGuardPhase::Veille)
	{
		OnRepliqueDeSecours.Broadcast(TEXT("Poste ferme. Repassez plus tard."));
	}

	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().SetTimer(
			MinuterieReconnexion, this, &AGuardSessionManager::TenterReconnexion,
			FMath::Max(DelaiReconnexion, 1.f), false);
	}
}

void AGuardSessionManager::TenterReconnexion()
{
	if (Sidecar && !Sidecar->EstConnecte())
	{
		UE_LOG(LogGardeFrontiere, Log, TEXT("Sidecar : nouvelle tentative"));
		Sidecar->Connecter(UrlSidecar);
	}
}

// -- Minuteries ----------------------------------------------------------

void AGuardSessionManager::ArmerAbandon()
{
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().SetTimer(
			MinuterieAbandon, this, &AGuardSessionManager::SurAbandon,
			FMath::Max(DelaiAbandon, 5.f), false);
	}
}

void AGuardSessionManager::SurAbandon()
{
	UE_LOG(LogGardeFrontiere, Warning, TEXT("Aucune interaction — abandon"));
	TerminerSession(EGuardFinDeSession::Timeout);
}

void AGuardSessionManager::AnnulerMinuteries()
{
	if (UWorld* Monde = GetWorld())
	{
		FTimerManager& T = Monde->GetTimerManager();
		T.ClearTimer(MinuterieAbandon);
		T.ClearTimer(MinuterieSortie);
		T.ClearTimer(MinuterieReconnexion);
	}
}
