#include "GuardSessionManager.h"

#include "GardeFrontiere.h"
#include "SidecarClient.h"
#include "LidarPresenceComponent.h"
#include "AgentVoiceComponent.h"
#include "AgentFaceComponent.h"
#include "GlitchComponent.h"
#include "AvatarSwitcherComponent.h"
#include "StampComponent.h"
#include "AudioBridge.h"
#include "TimerManager.h"
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

	Sidecar->Connecter(UrlSidecar);

	if (Presence)
	{
		Presence->OnPresenceDetectee.AddDynamic(this, &AGuardSessionManager::SurPresenceDetectee);
		Presence->OnPresencePerdue.AddDynamic(this, &AGuardSessionManager::SurPresencePerdue);
	}

	// Le glitch masque la substitution : on ne permute qu'une fois l'effet
	// installe, sinon le visiteur verrait l'avatar disparaitre puis
	// reapparaitre a l'ecran.
	if (Glitch)
	{
		Glitch->OnGlitchTermine.AddDynamic(this, &AGuardSessionManager::SurGlitchTermine);
	}

	// A chaque nouvel avatar, le composant d'expression doit recibler le
	// maillage facial : l'ancien vient d'etre detruit.
	if (Avatars)
	{
		Avatars->OnAvatarChange.AddDynamic(this, &AGuardSessionManager::SurAvatarChange);
	}

	ChangerPhase(EGuardPhase::Veille);
	UE_LOG(LogGardeFrontiere, Log, TEXT("Borne prete — en veille"));
}

void AGuardSessionManager::EndPlay(const EEndPlayReason::Type Raison)
{
	AnnulerMinuteries();
	if (Sidecar)
	{
		Sidecar->Deconnecter();
	}
	Super::EndPlay(Raison);
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

	Phase = Nouvelle;
	OnPhaseChangee.Broadcast(Nouvelle);
}

int32 AGuardSessionManager::TirerAvatar()
{
	if (NombreAvatars <= 1)
	{
		return 0;
	}

	int32 Index = FMath::RandRange(0, NombreAvatars - 1);

	// Sans cette precaution, le meme visage revient une fois sur trois —
	// assez pour que deux visiteurs successifs le remarquent.
	if (bEviterRepetitionAvatar && Index == IndexAvatarPrecedent)
	{
		Index = (Index + 1 + FMath::RandRange(0, NombreAvatars - 2)) % NombreAvatars;
	}

	IndexAvatarPrecedent = Index;
	return Index;
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
	IndexAvatarCourant = TirerAvatar();

	ChangerPhase(EGuardPhase::Accueil);

	if (Tampons)
	{
		Tampons->Masquer();
	}

	// Le glitch masque la substitution. La permutation elle-meme attend
	// OnGlitchTermine — a defaut d'effet, on permute tout de suite plutot
	// que de rester bloque.
	if (Glitch)
	{
		Glitch->Declencher();
	}
	else if (Avatars)
	{
		Avatars->Permuter();
	}

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

	if (Sidecar && Sidecar->EstConnecte())
	{
		Sidecar->SignalerAbsence();
	}

	TerminerSession(bApresVerdict
		? EGuardFinDeSession::Nominale
		: EGuardFinDeSession::Abandon);
}

// -- Sidecar -------------------------------------------------------------

void AGuardSessionManager::SurGlitchTermine()
{
	// L'effet a couvert la substitution : on peut permuter sans que le
	// visiteur voie l'avatar disparaitre.
	if (Avatars)
	{
		Avatars->Permuter();
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
}

void AGuardSessionManager::SurParoleDebut(const FString& Texte, EGuardEmotion Emotion)
{
	bIADisponible = true;

	if (Phase == EGuardPhase::Accueil)
	{
		ChangerPhase(EGuardPhase::Interrogatoire);
	}

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
