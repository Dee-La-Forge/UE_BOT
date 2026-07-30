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

#include "A2XSession.h"
#include "A2FProvider.h"
#include "ACETypes.h"   // FAudio2FaceEmotion — l'emotion imposee a A2F
#include "ACEAudioCurveSourceComponent.h"
#include "ACEBlueprintLibrary.h"
#include "ACERuntimeModule.h"
#include "AudioBridge.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

// Ouvre une session sans capteur, N secondes apres le demarrage.
//
// Une borne de mise au point n'a pas toujours son LiDAR : sur un poste de
// developpement, rien ne declenche jamais presence.detectee, et la borne
// reste en veille sans que ce soit une panne. Cette variable evite d'avoir
// a cliquer dans l'editeur — elle se pose dans DefaultEngine.ini, section
// [SystemSettings], donc sans recompiler ni toucher a la scene.
//
//   [SystemSettings]
//   gf.SessionAuto=3
//
// 0 (le defaut) = comportement normal : seul le capteur ouvre une session.
static TAutoConsoleVariable<float> CVarSessionAuto(
	TEXT("gf.SessionAuto"),
	0.f,
	TEXT("Ouvre une session N secondes apres le demarrage, sans capteur.\n")
	TEXT("Mise au point sans LiDAR. 0 = desactive."),
	ECVF_Default);

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

	// L'emotion definitive arrive APRES la replique, une fois le tag lu en
	// fin de generation. Personne ne s'y abonnait : le visage restait sur
	// l'emotion pressentie (« Stare ») envoyee avec parole.debut, et tout le
	// mecanisme Angry/Concerned/Happy des grammaires se perdait en route.
	Sidecar->OnEmotion.AddDynamic(this, &AGuardSessionManager::SurEmotion);

	// Les trames audio arrivent en binaire, hors du systeme de delegues
	// Blueprint : trop volumineuses pour y transiter. On les route
	// directement vers la voix.
	Sidecar->OnAudioRecu.AddLambda(
		[this](const TArray<uint8>& PCM16, int32 Taux)
		{
			// Hors replique, on ne joue RIEN — ni Audio2Face ni repli. Une
			// trame arrivant apres parole.fin, ou apres la fin de session,
			// est un reliquat de la replique close : la jouer par la voix de
			// repli pendant qu'ACE finit la precedente superposait deux
			// lectures — exactement les deux voix que bRepliqueEnCours doit
			// empecher. OuvrirSessionA2F posait deja ce garde, mais son
			// `false` etait indistinguable d'un « A2F absent », et le repli
			// jouait la trame sans condition.
			if (!bRepliqueEnCours || Phase == EGuardPhase::Veille)
			{
				// Ce rejet est SILENCIEUX par nature — et c'est
				// exactement ce qui rend « l'agent ne parle pas »
				// indiagnosticable : rien ne distingue une trame jetee
				// ici d'une trame jamais arrivee. On le dit, une fois
				// par replique refusee.
				if (!bRejetTrameSignale)
				{
					bRejetTrameSignale = true;
					UE_LOG(LogGardeFrontiere, Warning,
						TEXT("Audio rejete : %d octets recus hors replique ")
						TEXT("(bRepliqueEnCours=%d, phase=%s). Un parole.debut ")
						TEXT("a-t-il ete recu avant ?"),
						PCM16.Num(), bRepliqueEnCours ? 1 : 0,
						*StaticEnum<EGuardPhase>()->GetDisplayNameTextByValue((int64)Phase).ToString());
				}
				return;
			}
			bRejetTrameSignale = false;

			// Audio2Face d'abord : son composant JOUE le son en plus d'animer
			// le visage. Empiler la meme trame dans Voix ferait parler l'agent
			// deux fois, avec un decalage.
			const bool bA2F = OuvrirSessionA2F(Taux);

			if (bA2F)
			{
				const TArrayView<const int16> Echantillons(
					reinterpret_cast<const int16*>(PCM16.GetData()), PCM16.Num() / 2);

				// L'emotion decidee par le LLM voyage AVEC l'audio : A2F
				// l'applique au visage en meme temps qu'il calcule le
				// lipsync. Ce troisieme parametre est reste vide jusqu'au
				// 30/07/2026, et l'agent parlait donc sans expression —
				// voir docs/EMOTIONS-VS-LIPSYNC.md.
				SessionA2F->SendAudioSamples(
					Echantillons, false, EmotionPourA2F(), nullptr);

				// Entretien de la fin de lecture ESTIMEE (voir le membre) :
				// ancre au premier envoi de la replique, puis cumul des
				// durees. C'est la seule vue qu'on ait sur la lecture ACE.
				if (UWorld* Monde = GetWorld())
				{
					const double Maintenant = Monde->GetTimeSeconds();
					if (FinLectureAcePresumee < Maintenant)
					{
						FinLectureAcePresumee = Maintenant + GraceSuiviLecture;
					}
					FinLectureAcePresumee +=
						(PCM16.Num() / 2.0) / FMath::Max(Taux, 1);
				}

				// Le composant ACE est cense jouer le son qu'il renvoie. Tant
				// qu'il reste muet, on double par Voix pour entendre la
				// replique et voir si le visage s'anime malgre tout.
				if (!bDoublerVoixPourDiagnostic)
				{
					return;
				}
			}

			// Repli : sans Audio2Face, la borne parle quand meme, bouche
			// fermee. Une borne muette serait un echec ; une borne qui parle
			// sans articuler reste utilisable.
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

	// Mode d'inference d'Audio2Face, avant toute session.
	UACEBlueprintLibrary::OverrideA2F3DInferenceMode(bInferenceEnRafale);
	UE_LOG(LogGardeFrontiere, Log, TEXT("Audio2Face : inference %s"),
		bInferenceEnRafale ? TEXT("en rafale") : TEXT("bridee au temps reel"));

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

	// Mise au point sans LiDAR : voir CVarSessionAuto en tete de fichier.
	const float DelaiAuto = CVarSessionAuto.GetValueOnGameThread();
	if (DelaiAuto > 0.f)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("DIAGNOSTIC : gf.SessionAuto=%.1f — session ouverte sans capteur"),
			DelaiAuto);
		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().SetTimer(
				MinuterieReprise, this, &AGuardSessionManager::SurReprisePresenceAuto,
				DelaiAuto, false);
		}
	}
}

void AGuardSessionManager::SurReprisePresenceAuto()
{
	if (Phase == EGuardPhase::Veille)
	{
		DemarrerSession();
	}
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

	// Avant tout le reste : la session Audio2Face tient un pointeur sur le
	// composant de l'avatar, et l'avatar est detruit plus loin dans cette
	// meme sequence d'arret.
	FermerSessionA2F();

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
	// La POSTURE suit la session entiere : l'agent fait face au visiteur
	// jusqu'a son depart, verdict rendu ou non.
	const bool bEcouteVoulue = (Nouvelle != EGuardPhase::Veille);
	if (bEcouteVoulue != bEnEcoute)
	{
		bEnEcoute = bEcouteVoulue;
		AppliquerPosture();
		OnEcouteChangee.Broadcast(bEnEcoute);
	}

	// Le MICRO, lui, ne suit que les phases conversationnelles. Il se ferme
	// au verdict : l'echange est clos, et continuer d'ecouter relancait le
	// cycle a chaque parole du visiteur.
	//
	// La capture materielle reste ouverte en permanence : ouvrir un
	// peripherique audio prend des centaines de millisecondes, et les payer a
	// l'arrivee du visiteur lui couterait ses premiers mots.
	if (Micro)
	{
		ConversationEnCours() ? Micro->DemarrerEcoute() : Micro->ArreterEcoute();
	}
}

bool AGuardSessionManager::ConversationEnCours() const
{
	return Phase == EGuardPhase::Accueil || Phase == EGuardPhase::Interrogatoire;
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
	bRelanceFaite = false;
	bPanneAnnoncee = false;

	// Une session demarre : la reprise programmee n'a plus d'objet, et la
	// substitution differee sera rejouee a la fin de CETTE session.
	bSubstitutionDifferee = false;
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearTimer(MinuterieReprise);
	}

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
	// Seules les phases conversationnelles acceptent la parole du visiteur.
	//
	// Le garde ne portait que sur Veille, et le verdict rendu la borne
	// continuait d'ecouter : chaque nouvel enonce repartait au sidecar, qui
	// rendait un nouveau verdict, qui relancait le cycle. La phase oscillait
	// indefiniment entre Verdict et SortieZone — onze allers-retours releves
	// dans une seule session.
	//
	// Apres le verdict, l'echange est clos. Le visiteur peut parler, on
	// n'ecoute plus.
	if (!ConversationEnCours() || Echantillons.Num() == 0)
	{
		return;
	}

	// La borne ne doit pas s'entendre elle-meme. A volume d'exposition, la
	// voix TTS captee par le micro est segmentee par Silero comme n'importe
	// quelle parole, passait le seuil de niveau, et repartait au sidecar
	// comme « reponse » du visiteur : l'interrogatoire pouvait avancer tout
	// seul, sur les propres repliques de la borne. Tout segment clos pendant
	// que l'agent est audible est donc jete, plus une marge apres la fin de
	// lecture, le temps que la reverberation de la salle retombe.
	const double Maintenant = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const bool bFenetreSourde =
		AgentAudible()
		|| (Maintenant - InstantAgentAudible < MargeEchoApresReplique);
	if (bFenetreSourde)
	{
		// Le segment peut etre l'echo de la borne (le cas normal de cette
		// fenetre) comme une VRAIE reponse tombee dans la queue de
		// lecture : on le perd, mais on ne tranche pas a l'aveugle.
		// L'abandon est repousse — quelque chose s'est manifeste — et la
		// relance est REPOUSSEE, pas annulee : l'annuler laissait un
		// visiteur reellement muet sans rappel jusqu'a l'abandon (l'echo
		// de chaque replique la tuait), la laisser courir grondait celui
		// qui venait de repondre. Le silence sera rejuge une pleine
		// fenetre apres ce signal.
		// (InstantAgentAudible n'est PLUS estampille ici : le suivi de
		// lecture fait foi, et l'estampille sur rejet repoussait la marge
		// devant un visiteur qui reessayait.)
		ArmerAbandon();
		if (ConversationEnCours() && !bRelanceFaite)
		{
			if (UWorld* Monde = GetWorld())
			{
				Monde->GetTimerManager().SetTimer(
					MinuterieRelance, this, &AGuardSessionManager::SurSilenceVisiteur,
					FMath::Max(DelaiReponseVisiteur, 1.f), false);
			}
		}
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Segment micro ignore : fenetre sourde (lecture ou marge de %.1f s)"),
			MargeEchoApresReplique);
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

		// Le visiteur a repondu : le silence en cours est rompu, et une
		// future relance redevient possible.
		bRelanceFaite = false;
		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().ClearTimer(MinuterieRelance);
		}

		// Meme surveillance qu'a l'ouverture : un enonce transmis doit
		// produire une replique dans un delai borne, sinon le sidecar est
		// considere mort malgre sa socket ouverte.
		ArmerSurveillanceIA();
	}
	else
	{
		// Mode degrade : le visiteur PARLE — on repousse l'abandon, qui
		// tombait a 30 s pile en pleine phrase. Et « Repetez. » part au plus
		// une fois par fenetre : chaque segment VAD declenchait le sien, et
		// un visiteur volubile se faisait rabrouer en rafale.
		ArmerAbandon();
		if (Maintenant - InstantDernierRepetez >= 6.0)
		{
			InstantDernierRepetez = Maintenant;
			DireRepliqueDeSecours(TEXT("Repetez."));
		}
	}
}

void AGuardSessionManager::ArmerSuiviLecture()
{
	if (UWorld* Monde = GetWorld())
	{
		InstantAgentAudible = Monde->GetTimeSeconds();
		bLectureObservee = false;
		Monde->GetTimerManager().SetTimer(
			MinuterieSuiviLecture, this, &AGuardSessionManager::SurSuiviLecture,
			0.2f, true);
	}
}

void AGuardSessionManager::DireRepliqueDeSecours(const FString& Texte)
{
	OnRepliqueDeSecours.Broadcast(Texte);
	ArmerSuiviLecture();   // les secours n'ont pas de parole.fin
}

void AGuardSessionManager::CouperLecture()
{
	bRepliqueEnCours = false;
	FermerSessionA2F();

	// La lecture est coupee : l'estimation de fin ne vaut plus rien, et la
	// laisser courir garderait le micro sourd sur du silence.
	FinLectureAcePresumee = -1.0e9;

	// EndAudioSamples signale la fin du flux, il n'arrete PAS la lecture :
	// le composant ACE continue de jouer ce qu'il a en tampon, et
	// Voix->Interrompre() ne coupe que le repli. Sans le Stop, une
	// replique interrompue se finissait toute seule devant une zone vide.
	if (Avatars)
	{
		if (UACEAudioCurveSourceComponent* Source = Avatars->TrouverComposantACE())
		{
			Source->Stop();
		}
	}
	if (Voix)
	{
		Voix->Interrompre();
	}
}

void AGuardSessionManager::SurSuiviLecture()
{
	UWorld* Monde = GetWorld();
	if (!Monde)
	{
		return;
	}

	if (AgentAudible())
	{
		// La lecture court : la marge anti-echo repart d'ici.
		bLectureObservee = true;
		InstantAgentAudible = Monde->GetTimeSeconds();
		return;
	}

	// Rien d'audible. Tant qu'on n'a JAMAIS entendu la lecture, c'est
	// probablement qu'elle n'a pas encore commence — sur Audio2Face, le son
	// part 0,7-1,4 s apres parole.fin. S'arreter a la premiere sonde muette
	// figeait la marge sur parole.fin, et l'echo de chaque replique courte
	// repassait au travers. On patiente donc le delai de grace ; si rien ne
	// vient, il n'y avait rien a suivre. (Tant que rien n'a ete entendu,
	// InstantAgentAudible date de l'armement : c'est l'origine du delai.)
	if (!bLectureObservee
		&& Monde->GetTimeSeconds() - InstantAgentAudible < GraceSuiviLecture)
	{
		return;
	}

	// Fin reelle du son (ou lecture jamais venue) : la derniere estampille
	// fait desormais foi, le sondage n'a plus rien a suivre.
	Monde->GetTimerManager().ClearTimer(MinuterieSuiviLecture);
}

bool AGuardSessionManager::AgentAudible()
{
	// Le flux est ouvert : des trames arrivent ou vont arriver.
	if (bRepliqueEnCours)
	{
		return true;
	}

	// parole.fin clot le FLUX, pas la LECTURE : ACE comme Voix continuent
	// de jouer ce qu'ils ont en tampon, parfois plusieurs secondes.
	if (Voix && Voix->EstEnTrainDeParler())
	{
		return true;
	}

	// Cote ACE, pas d'etat de lecture public : on compare a la fin
	// estimee, entretenue a chaque trame envoyee (voir le membre).
	if (UWorld* Monde = GetWorld())
	{
		if (Monde->GetTimeSeconds() < FinLectureAcePresumee)
		{
			return true;
		}
	}

	return false;
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
	CouperLecture();

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
	// « Sans temoin » se VERIFIE : apres un abandon, le visiteur est souvent
	// encore devant la borne. Glitcher sous ses yeux montrerait le tour, et
	// l'ignorer ensuite le planterait devant une borne morte — le capteur ne
	// rend que des fronts, aucun nouveau presence.detectee ne viendra tant
	// que la zone ne se vide pas. Zone occupee : substitution differee, et
	// nouvelle session reprogrammee.
	//
	// Declenche apres le Arreter() du nettoyage ci-dessus, sinon on eteindrait
	// l'effet qu'on vient d'allumer.
	const bool bZoneVide = !Presence || !Presence->bPresent;
	if (bZoneVide)
	{
		LancerSubstitution();
	}
	else
	{
		bSubstitutionDifferee = true;
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Zone encore occupee — substitution differee, nouvelle session dans %.0f s"),
			DelaiReprisePresence);
		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().SetTimer(
				MinuterieReprise, this, &AGuardSessionManager::SurReprisePresence,
				FMath::Max(DelaiReprisePresence, 1.f), false);
		}
	}
}

TOptional<FAudio2FaceEmotion> AGuardSessionManager::EmotionPourA2F() const
{
	const EGuardEmotion Emotion = Visage ? Visage->EmotionCourante : EGuardEmotion::Neutral;

	// Neutral : rien d'impose. A2F garde son jeu de visage naturel.
	if (Emotion == EGuardEmotion::Neutral)
	{
		return NullOpt;
	}

	FAudio2FaceEmotion Parametres;
	FAudio2FaceEmotionOverride& O = Parametres.EmotionOverrides;

	switch (Emotion)
	{
	case EGuardEmotion::Angry:
		O.bOverrideAnger = true;
		O.Anger = 0.75f;
		break;

	case EGuardEmotion::Concerned:
		// Le doute d'un garde-frontiere, pas la peur : Fear en retrait,
		// une pointe de Disgust pour le pli dedaigneux du visage.
		O.bOverrideFear = true;
		O.Fear = 0.35f;
		O.bOverrideDisgust = true;
		O.Disgust = 0.25f;
		break;

	case EGuardEmotion::Happy:
		// Un garde-frontiere ne se rejouit pas : il approuve. Joy modere,
		// releve de Cheekiness — la satisfaction un peu narquoise de
		// celui qui laisse passer.
		O.bOverrideJoy = true;
		O.Joy = 0.5f;
		O.bOverrideCheekiness = true;
		O.Cheekiness = 0.3f;
		break;

	case EGuardEmotion::Stare:
	default:
		// L'etat par defaut de la persona : froid, jaugeant. Aucune des
		// dix emotions de A2F ne le nomme, et c'est normal — c'est une
		// ABSENCE d'expression, tenue. On l'obtient en bridant la force
		// globale plutot qu'en imposant un sentiment qu'il n'a pas.
		Parametres.OverallEmotionStrength = 0.15f;
		break;
	}

	return Parametres;
}

void AGuardSessionManager::LancerSubstitution()
{
	if (Glitch)
	{
		Glitch->Declencher();   // la permutation suivra OnGlitchTermine
	}
	else if (Avatars)
	{
		Avatars->Permuter();
	}
}

void AGuardSessionManager::SurReprisePresence()
{
	// La zone a pu se vider, ou une session demarrer, entre l'armement et
	// l'echeance : on ne force rien dans ces cas-la.
	if (Phase == EGuardPhase::Veille && Presence && Presence->bPresent)
	{
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Visiteur toujours present au retour en veille — nouvelle session"));
		DemarrerSession();
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
		// La zone se vide en veille : si une substitution attendait le
		// depart du temoin (fin de session zone occupee), c'est le moment
		// de la jouer — sans elle, le visiteur suivant retrouverait le
		// meme visage, ce que toute la mecanique cherche a eviter.
		if (bSubstitutionDifferee)
		{
			bSubstitutionDifferee = false;
			if (UWorld* Monde = GetWorld())
			{
				Monde->GetTimerManager().ClearTimer(MinuterieReprise);
			}
			LancerSubstitution();
		}
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

bool AGuardSessionManager::OuvrirSessionA2F(int32 Taux)
{
	// Une session par replique, rouverte si le taux change — ce qui
	// n'arriverait qu'en changeant de voix TTS.
	if (SessionA2F.IsValid() && TauxSessionA2F == Taux)
	{
		return true;
	}

	// Hors replique, aucune ouverture. Une trame arrivant apres parole.fin
	// est un reliquat de la replique qui vient de se clore : lui ouvrir une
	// session la ferait jouer par-dessus celle qui s'entend encore.
	if (!bRepliqueEnCours)
	{
		return false;
	}

	IACEAnimDataConsumer* Consommateur = Avatars ? Avatars->TrouverConsommateurACE() : nullptr;
	if (!Consommateur)
	{
		return false;   // pas d'avatar, ou plugin absent : repli sur Voix
	}

	// Le fournisseur par defaut du plugin est "RemoteA2F" : il tente de
	// joindre le cloud NVIDIA et refuse la session au bout de dix secondes
	// d'attente. Une borne d'exposition n'a pas de reseau, et n'en veut pas.
	//
	// On cherche donc un fournisseur LOCAL. Les modeles s'enregistrent sous
	// "LocalA2F-<nom>" — et seulement s'ils ont pu charger leurs poids : en
	// trouver un vaut donc test de disponibilite.
	FName Nom = FournisseurA2F;

	if (Nom.IsNone())
	{
		for (const FName& Candidat : UACEBlueprintLibrary::GetAvailableA2FProviderNames())
		{
			if (Candidat.ToString().StartsWith(TEXT("LocalA2F")))
			{
				Nom = Candidat;
				break;
			}
		}
	}

	if (Nom.IsNone())
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Audio2Face : aucun fournisseur local — le plugin de modele ")
			TEXT("(NvAudio2FaceJames) est-il installe et ses poids charges ?"));
		return false;
	}

	IA2FProvider* Fournisseur = GetProviderFromName(Nom);
	if (!Fournisseur)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Audio2Face : fournisseur '%s' introuvable"), *Nom.ToString());
		return false;
	}

	// Mono, PCM16 : ce que le sidecar envoie. La session reechantillonne
	// elle-meme vers les 16 kHz qu'attend le reseau.
	SessionA2F = MakePimpl<FAudio2XSession>(Fournisseur, 1, static_cast<uint32>(Taux), 2);

	if (!SessionA2F->StartSession(Consommateur))
	{
		UE_LOG(LogGardeFrontiere, Warning, TEXT("Audio2Face : ouverture de session refusee"));
		SessionA2F.Reset();
		return false;
	}

	TauxSessionA2F = Taux;
	UE_LOG(LogGardeFrontiere, Log,
		TEXT("Audio2Face : session ouverte a %d Hz sur '%s'"), Taux, *Nom.ToString());
	return true;
}

void AGuardSessionManager::FermerSessionA2F()
{
	if (!SessionA2F.IsValid())
	{
		return;
	}

	// EndAudioSamples signale la fin du flux sans couper la lecture : le
	// composant ACE finit de jouer et d'animer ce qu'il a deja recu.
	SessionA2F->EndAudioSamples();
	SessionA2F.Reset();
	TauxSessionA2F = 0;
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

		// La panne n'etait detectee qu'au niveau CONNEXION. Un sidecar gele
		// — processus bloque, socket TCP restee ouverte — laissait
		// EstConnecte() vrai pour toujours : chaque visiteur affrontait
		// DelaiAbandon secondes de silence total, sans jamais basculer en
		// mode degrade. On attend donc une reponse dans un delai borne —
		// mais le delai LONG, celui de l'intro : elle est generee sans
		// cache de prompt et n'a pas a etre jugee comme les suivantes.
		ArmerSurveillanceIA(/*bPremiere=*/true);
	}
	else
	{
		// Mode degrade : la borne accueille quand meme.
		DireRepliqueDeSecours(TEXT("Papiers. Garde-frontiere."));
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
	// Evenement en vol au moment du depart du visiteur : le session.reset de
	// TerminerSession croise ce que le sidecar avait deja mis sur la socket.
	// Sans ce garde, un parole.debut tardif remettait bRepliqueEnCours a
	// vrai en Veille — et l'agent jouait la replique du visiteur parti
	// devant une zone vide.
	if (Phase == EGuardPhase::Veille)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("parole.debut recu en veille — evenement tardif ignore"));
		return;
	}

	bIADisponible = true;

	// Le sidecar repond : une future panne meritera une nouvelle annonce.
	bPanneAnnoncee = false;

	// Trace de bout en bout : c'est le premier signe, cote Unreal, qu'une
	// replique arrive. Sans elle, « l'agent reste muet » ne disait pas si
	// le sidecar avait parle ou si Unreal n'avait rien recu.
	UE_LOG(LogGardeFrontiere, Log, TEXT("Replique recue : « %s »"), *Texte);

	// Le sidecar vient de repondre : la surveillance a rempli son office.
	AnnulerSurveillanceIA();

	// L'agent parle : la fenetre de silence du visiteur ne court plus.
	// Elle sera re-armee a la fin de la replique.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearTimer(MinuterieRelance);
	}

	// Nouvelle replique : on clot le flux precedent s'il en restait un, puis
	// on autorise l'ouverture d'un nouveau. Hors de cette fenetre, aucune
	// trame ne peut ouvrir de session — c'est ce qui empeche une trame en
	// retard d'en creer une seconde pendant que la premiere joue encore.
	FermerSessionA2F();
	bRepliqueEnCours = true;

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
	if (Phase == EGuardPhase::Veille)
	{
		return;   // evenement tardif : la session est deja close
	}

	// La replique est finie : on ferme, et il FAUT fermer ici.
	//
	// EndAudioSamples est ce qui vide la file. A2XSession retient les
	// derniers echantillons tant qu'ils n'atteignent pas son minimum, et ne
	// les envoie qu'a la fermeture. Differer celle-ci laissait la fin de
	// chaque phrase coincee — voix coupee, animation figee sur sa derniere
	// trame recue.
	bRepliqueEnCours = false;
	FermerSessionA2F();

	// La lecture continue apres ce parole.fin (tampon ACE, parfois
	// plusieurs secondes) sans aucun evenement de fin : on la SUIT par
	// sondage, pour que la marge anti-echo parte de la fin reelle du son
	// et non du parole.fin. Une marge assise sur parole.fin laissait
	// passer tout echo clos apres la fin de lecture.
	ArmerSuiviLecture();

	ArmerAbandon();

	// C'est maintenant au visiteur de parler. S'il ne dit rien d'ici
	// DelaiReponseVisiteur, l'agent le relancera — une fois.
	if (ConversationEnCours() && !bRelanceFaite)
	{
		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().SetTimer(
				MinuterieRelance, this, &AGuardSessionManager::SurSilenceVisiteur,
				FMath::Max(DelaiReponseVisiteur, 1.f), false);
		}
	}
}

void AGuardSessionManager::SurVerdict(EGuardVerdict Decision)
{
	// Un verdict en vol au depart du visiteur arrivait apres TerminerSession
	// et refabriquait une phase Verdict sans visiteur ni minuterie : borne
	// figee, visiteur suivant ignore jusqu'a ce qu'un depart la debloque.
	if (Phase == EGuardPhase::Veille)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("verdict recu en veille — evenement tardif ignore"));
		return;
	}

	DernierVerdict = Decision;
	ChangerPhase(EGuardPhase::Verdict);

	if (Tampons)
	{
		Tampons->AfficherVerdict(Decision);
	}
	OnVerdictRendu.Broadcast(Decision);

	// Filet : si le session.terminee qui doit suivre se perdait, aucune
	// minuterie n'etait armee sur ce chemin et la borne restait en Verdict
	// jusqu'au depart du visiteur suivant.
	ArmerAbandon();
}

void AGuardSessionManager::SurSessionTerminee()
{
	if (Phase == EGuardPhase::Veille)
	{
		// Tardif : armer la sortie ferait afficher « quittez la zone »
		// devant un hall vide, puis glitch et permutation gratuits.
		return;
	}

	// On laisse le visiteur lire son tampon avant de le prier de sortir.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().SetTimer(
			MinuterieSortie, this, &AGuardSessionManager::SurDelaiSortie,
			FMath::Max(DelaiAvantSortie, 0.01f), false);
	}
}

void AGuardSessionManager::SurEmotion(EGuardEmotion Emotion)
{
	if (Phase == EGuardPhase::Veille)
	{
		return;
	}

	if (Visage)
	{
		Visage->AppliquerEmotion(Emotion);
	}
	OnEmotionChangee.Broadcast(Emotion);
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
	// Vrai seulement a la TRANSITION : les repassages (echec de
	// reconnexion toutes les DelaiReconnexion secondes) rentrent ici avec
	// bIADisponible deja faux.
	const bool bPremierePanne = bIADisponible;
	bIADisponible = false;
	UE_LOG(LogGardeFrontiere, Error, TEXT("IA indisponible : %s"), *Raison);

	if (bPremierePanne)
	{
		// Le flux est mort avec le sidecar : le parole.fin de la replique
		// en cours ne viendra jamais (bRepliqueEnCours resterait
		// verrouille, micro sourd), et la replique morte continuait de
		// jouer sous l'annonce de panne. On coupe tout — mais UNIQUEMENT
		// a la transition : aux repassages, la seule voix restante est
		// celle des secours, et ce Stop la tronquait en plein mot a
		// chaque cycle de reconnexion.
		CouperLecture();
	}

	// Une borne muette avec un visiteur planté devant est le seul echec
	// vraiment couteux. On parle, meme mal — mais UNE fois par panne :
	// sans le garde, l'agent repetait « Poste ferme » a chaque cycle. Le
	// suivi de lecture s'arme AVEC la parole, jamais sans : arme a chaque
	// repassage, il ouvrait une fenetre sourde fantome de 1,2 s toutes
	// les 5 s — un quart du temps de panne sourd a de la vraie parole.
	if (Phase != EGuardPhase::Veille && !bPanneAnnoncee)
	{
		bPanneAnnoncee = true;
		DireRepliqueDeSecours(TEXT("Poste ferme. Repassez plus tard."));
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

void AGuardSessionManager::SurSilenceVisiteur()
{
	// La fenetre a pu se fermer entre l'armement et l'echeance.
	if (!ConversationEnCours() || bRelanceFaite)
	{
		return;
	}

	if (Sidecar && Sidecar->EstConnecte())
	{
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Visiteur muet depuis %.0f s — relance"), DelaiReponseVisiteur);
		bRelanceFaite = true;
		Sidecar->SignalerSilence();
		ArmerSurveillanceIA();
	}
	// Sidecar absent : rien. La replique de secours d'une relance ratee
	// n'apporterait rien de plus que l'abandon qui suivra.
}

// -- Surveillance applicative du sidecar ----------------------------------

void AGuardSessionManager::ArmerSurveillanceIA(bool bPremiere)
{
	if (UWorld* Monde = GetWorld())
	{
		// La premiere replique n'a aucun cache de prompt a reutiliser :
		// elle est structurellement plus lente. Voir DelaiPremiereReponse.
		const float Delai = bPremiere ? DelaiPremiereReponse : DelaiReponseSidecar;
		Monde->GetTimerManager().SetTimer(
			MinuterieSurveillanceIA, this, &AGuardSessionManager::SurSilenceIA,
			FMath::Max(Delai, 2.f), false);
	}
}

void AGuardSessionManager::AnnulerSurveillanceIA()
{
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearTimer(MinuterieSurveillanceIA);
	}
}

void AGuardSessionManager::SurSilenceIA()
{
	if (!Sidecar)
	{
		return;
	}

	UE_LOG(LogGardeFrontiere, Error,
		TEXT("Sidecar : connecte mais muet depuis %.0f s — connexion consideree morte"),
		DelaiReponseSidecar);

	// Fermer la socket zombie, puis derouler exactement le chemin d'une
	// panne de connexion : replique de secours si un visiteur est la, et
	// boucle de reconnexion — qui retombera sur OnConnectionError tant que
	// le processus Python ne repond pas.
	Sidecar->Deconnecter();
	SurPanneIA(TEXT("sidecar muet (delai de reponse depasse)"));
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
		T.ClearTimer(MinuterieSurveillanceIA);
		T.ClearTimer(MinuterieRelance);
		T.ClearTimer(MinuterieSuiviLecture);

		// MinuterieReprise aussi : TerminerSession la REARME apres cet
		// appel si la zone est encore occupee — l'effacer d'abord evite
		// qu'une reprise armee par une session precedente survive.
		T.ClearTimer(MinuterieReprise);

		// PAS MinuterieReconnexion : elle n'appartient pas a la session.
		// L'effacer ici tuait la boucle de reconnexion pour de bon — la
		// chaine panne -> reconnexion -> echec -> re-armement ne survit que
		// par SurPanneIA, et une fin de session tombant dans la fenetre
		// d'attente laissait la borne en mode degrade jusqu'au redemarrage,
		// meme sidecar revenu. Elle est purgee dans EndPlay, par
		// ClearAllTimersForObject.
	}
}
