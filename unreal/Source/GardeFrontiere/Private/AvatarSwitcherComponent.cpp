#include "AvatarSwitcherComponent.h"

#include "GardeFrontiere.h"
#include "Algo/Find.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

#include "ACEAudioCurveSourceComponent.h"

UAvatarSwitcherComponent::UAvatarSwitcherComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// Les trois agents, en references DIFFEREES.
	//
	// Surtout pas ConstructorHelpers::FClassFinder ici. Il charge de facon
	// synchrone, dans le constructeur, donc a la creation du CDO — pendant
	// l'initialisation du module, avant que tous les plugins soient debout.
	// Charger BP_AgentGermain tire alors toutes ses dependances, dont
	// ABP_MH_LiveLink, dont les imports reclament /Script/LiveLink :
	//
	//   VerifyImport: Failed to find script package for import object
	//   'Package /Script/LiveLink'
	//
	// L'AnimBP perdait ses broches et echouait a compiler, seize erreurs a
	// chaque demarrage de l'editeur. ConstructorHelpers convient a un asset
	// simple, pas a un MetaHuman qui traine des centaines de dependances.
	//
	// Les chemins sont donc resolus dans BeginPlay, quand tout est charge.
	AvatarsParDefaut = {
		FSoftClassPath(TEXT("/Game/MetaHumans/AgentGermain/BP_AgentGermain.BP_AgentGermain_C")),
		FSoftClassPath(TEXT("/Game/MetaHumans/AgentLouise/BP_AgentLouise.BP_AgentLouise_C")),
		FSoftClassPath(TEXT("/Game/MetaHumans/AgentTrinity/BP_AgentTrinity.BP_AgentTrinity_C")),
	};
}

void UAvatarSwitcherComponent::BeginPlay()
{
	Super::BeginPlay();

	// Le tableau renseigne dans le niveau prime toujours : ces chemins ne
	// sont qu'un filet pour un GuardSessionManager pose sans reglage.
	if (ClassesAvatars.Num() > 0)
	{
		return;
	}

	for (const FSoftClassPath& Chemin : AvatarsParDefaut)
	{
		if (UClass* Classe = Chemin.TryLoadClass<AActor>())
		{
			ClassesAvatars.Add(Classe);
		}
		else if (!Chemin.IsNull())
		{
			UE_LOG(LogGardeFrontiere, Warning,
				TEXT("Avatars : %s introuvable — avatar ignore"), *Chemin.ToString());
		}
	}

	UE_LOG(LogGardeFrontiere, Log,
		TEXT("Avatars : %d classe(s) chargee(s) par defaut"), ClassesAvatars.Num());
}

void UAvatarSwitcherComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	DetruireCourant();
	Super::EndPlay(Raison);
}

int32 UAvatarSwitcherComponent::Tirer() const
{
	// On ne tire QUE parmi les entrees renseignees.
	//
	// Un tableau de trois cases dont une seule est remplie faisait tomber
	// le tirage sur une case vide deux fois sur trois — et aucun personnage
	// n'apparaissait. C'est le defaut du Blueprint d'origine, que je
	// reproduisais fidelement.
	TArray<int32> Valides;
	Valides.Reserve(ClassesAvatars.Num());
	for (int32 i = 0; i < ClassesAvatars.Num(); ++i)
	{
		if (ClassesAvatars[i])
		{
			Valides.Add(i);
		}
	}

	if (Valides.Num() == 0)
	{
		return INDEX_NONE;
	}
	if (Valides.Num() == 1)
	{
		IndexPrecedent = Valides[0];
		return Valides[0];
	}

	// Avec trois avatars, un tirage libre redonne le meme une fois sur
	// trois. On decale plutot que de retirer en boucle : temps constant,
	// et distribution uniforme sur les autres.
	int32 Rang = FMath::RandRange(0, Valides.Num() - 1);
	if (bEviterRepetition && Valides[Rang] == IndexPrecedent)
	{
		Rang = (Rang + 1 + FMath::RandRange(0, Valides.Num() - 2)) % Valides.Num();
	}

	IndexPrecedent = Valides[Rang];
	return IndexPrecedent;
}

void UAvatarSwitcherComponent::DetruireCourant()
{
	if (AvatarCourant)
	{
		AvatarCourant->Destroy();
		AvatarCourant = nullptr;
	}
}

AActor* UAvatarSwitcherComponent::Permuter()
{
	const int32 Index = Tirer();
	if (Index == INDEX_NONE)
	{
		// Message explicite : un tableau non vide mais peuple de cases
		// nulles produit exactement le meme symptome qu'un tableau vide —
		// aucun personnage — et le diagnostic n'a rien d'evident.
		UE_LOG(LogGardeFrontiere, Error,
			TEXT("Avatars : aucune classe valide dans ClassesAvatars ")
			TEXT("(%d entree(s), toutes vides). Assigner au moins BP_AgentGermain."),
			ClassesAvatars.Num());
		return nullptr;
	}
	return Spawner(Index);
}

AActor* UAvatarSwitcherComponent::Spawner(int32 Index)
{
	if (!ClassesAvatars.IsValidIndex(Index) || !ClassesAvatars[Index])
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Avatars : index %d invalide"), Index);
		return nullptr;
	}

	UWorld* Monde = GetWorld();
	if (!Monde)
	{
		return nullptr;
	}

	DetruireCourant();

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Nouveau = Monde->SpawnActor<AActor>(
		ClassesAvatars[Index], TransformSpawn, Params);

	if (!Nouveau)
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Avatars : echec du spawn (index %d)"), Index);
		return nullptr;
	}

	// Le retrait se fait APRES le spawn, et non entre SpawnActorDeferred et
	// FinishSpawning comme je l'avais d'abord ecrit : les composants d'un
	// Blueprint naissent dans le Simple Construction Script, execute par
	// FinishSpawning. Avant lui, seuls les composants natifs existent — et
	// ConvaiChatbot n'en est pas un. Il n'y a donc pas de fenetre avant
	// BeginPlay, et le chercher la revenait a ne rien trouver.
	//
	// La capture micro ne demarre qu'a la frame suivante — 40 ms apres le
	// spawn dans les traces. Detruire le composant ici, dans la meme pile
	// d'appels, la devance.
	// Filet de securite pour les avatars qui descendraient encore du God
	// Blueprint. BP_AgentGermain, lui, a ete reparente sur Character : il
	// n'apporte plus rien a retirer, et c'est tant mieux.
	if (bRetirerConvaiConversationnel)
	{
		RetirerConvaiConversationnel(Nouveau);
	}

	if (bPreparerAudio2Face)
	{
		PreparerAudio2Face(Nouveau);
	}

	AvatarCourant = Nouveau;

	IndexCourant = Index;
	UE_LOG(LogGardeFrontiere, Log, TEXT("Avatars : %s en place (index %d)"),
		*AvatarCourant->GetName(), Index);

	OnAvatarChange.Broadcast(AvatarCourant, Index);
	return AvatarCourant;
}

int32 UAvatarSwitcherComponent::RetirerConvaiConversationnel(AActor* Avatar) const
{
	if (!Avatar)
	{
		return 0;
	}

	// Identification par NOM de classe, et non par type : le module
	// GardeFrontiere ne depend pas de Convai et n'a pas a en dependre. Le
	// plugin n'est garde que pour ses animations et doit pouvoir disparaitre
	// sans qu'une ligne d'ici ne bouge — un include suffirait a rendre la
	// compilation impossible le jour ou on le retire.
	static const FName Conversationnels[] =
	{
		TEXT("ConvaiChatbotComponent"),
	};

	TArray<UActorComponent*> Composants;
	Avatar->GetComponents(Composants);

	int32 NbRetires = 0;

	for (UActorComponent* Composant : Composants)
	{
		if (!Composant)
		{
			continue;
		}

		// On remonte la hierarchie : un Blueprint peut deriver du composant
		// Convai plutot que l'utiliser tel quel, et le nom exact changerait.
		for (const UClass* Classe = Composant->GetClass(); Classe; Classe = Classe->GetSuperClass())
		{
			if (Algo::Find(Conversationnels, Classe->GetFName()) == nullptr)
			{
				continue;
			}

			UE_LOG(LogGardeFrontiere, Log,
				TEXT("Avatars : %s retire de %s — l'IA est locale"),
				*Composant->GetClass()->GetName(), *Avatar->GetName());

			// Detruire ne suffit pas : la variable Blueprint continue de
			// pointer sur le composant mourant. L'ubergraph de
			// BP_ConvaiCharacterBase l'interroge en boucle et chaque lecture
			// produit une erreur d'execution — inoffensive, mais repetee
			// toutes les 100 ms, elle noierait les vraies.
			//
			// On annule donc toute propriete de l'acteur qui le designe,
			// avant de le detruire. Le noeud IsValid du Blueprint repond
			// alors faux proprement, ce qui est exactement ce qu'il attend.
			for (TFieldIterator<FObjectProperty> It(Avatar->GetClass()); It; ++It)
			{
				FObjectProperty* Propriete = *It;
				if (Propriete->GetObjectPropertyValue_InContainer(Avatar) == Composant)
				{
					Propriete->SetObjectPropertyValue_InContainer(Avatar, nullptr);
				}
			}

			Composant->DestroyComponent();
			++NbRetires;
			break;
		}
	}

	// Ne rien trouver est desormais le cas NOMINAL : un avatar reparente sur
	// Character n'embarque plus de composant conversationnel. On le dit en
	// Log, pas en Warning.
	//
	// La trace reste utile : c'est elle qui, un jour, revelera qu'un avatar
	// nouvellement ajoute descend encore du God Blueprint. La premiere
	// version de ce code n'en avait pas et cherchait les composants trop tot
	// — elle n'en trouvait aucun, sans rien dire, ce qui a coute un cycle de
	// compilation et de test complet.
	if (NbRetires == 0)
	{
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Avatars : rien de conversationnel sur %s (%d composants) — deja propre."),
			*Avatar->GetName(), Composants.Num());
	}

	return NbRetires;
}

UACEAudioCurveSourceComponent* UAvatarSwitcherComponent::TrouverComposantACE() const
{
	return AvatarCourant
		? AvatarCourant->FindComponentByClass<UACEAudioCurveSourceComponent>()
		: nullptr;
}

IACEAnimDataConsumer* UAvatarSwitcherComponent::TrouverConsommateurACE() const
{
	return TrouverComposantACE();
}

void UAvatarSwitcherComponent::PreparerAudio2Face(AActor* Avatar) const
{
	if (!Avatar)
	{
		return;
	}

	// -- Le composant ACE ------------------------------------------------
	//
	// Instancie par nom de classe, comme le retrait Convai. ATTENTION : cela
	// ne rend PAS le module compilable sans le plugin NVIDIA — l'include
	// direct de ACEAudioCurveSourceComponent.h (TrouverComposantACE) et les
	// dependances ACERuntime/ACECore du Build.cs sont dures. Seul Silero est
	// reellement optionnel. L'instanciation par nom ne sert ici qu'au repli
	// A L'EXECUTION : plugin present mais modele absent, la borne parle et
	// entend encore, seule la bouche reste immobile.
	UClass* ClasseACE = FindObject<UClass>(
		nullptr, TEXT("/Script/ACERuntime.ACEAudioCurveSourceComponent"));

	if (!ClasseACE)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Avatars : ACEAudioCurveSourceComponent introuvable — le plugin ")
			TEXT("NV_ACE_Reference est-il installe ? La bouche restera immobile."));
		return;
	}

	// Un avatar peut deja le porter si son Blueprint a ete edite a la main.
	if (Avatar->GetComponentByClass(ClasseACE) == nullptr)
	{
		UActorComponent* Ace = NewObject<UActorComponent>(Avatar, ClasseACE, TEXT("ACEAudioCurveSource"));
		if (Ace)
		{
			// Attacher AVANT d'enregistrer : un USceneComponent non attache
			// se retrouve a l'origine du monde, et le son de l'agent sortirait
			// d'ailleurs que de sa bouche.
			if (USceneComponent* Scene = Cast<USceneComponent>(Ace))
			{
				Scene->SetupAttachment(Avatar->GetRootComponent());
			}
			Ace->RegisterComponent();

			UE_LOG(LogGardeFrontiere, Log,
				TEXT("Avatars : ACEAudioCurveSource pose sur %s"), *Avatar->GetName());
		}
	}

	// -- L'AnimBP facial -------------------------------------------------
	//
	// Face_AnimBP porte le noeud ApplyACEAnimation et la correspondance
	// ARKit. Il remplace Convai_MetaHuman_FaceAnim, qui ne pilotait plus rien
	// depuis le retrait du chatbot — et dont les courbes d'emotion
	// n'existaient de toute facon pas sous les noms attendus.
	if (AnimBPFacial.IsNull())
	{
		return;
	}

	USkeletalMeshComponent* Visage = nullptr;
	{
		TArray<USkeletalMeshComponent*> Maillages;
		Avatar->GetComponents<USkeletalMeshComponent>(Maillages);
		for (USkeletalMeshComponent* M : Maillages)
		{
			if (M && M->GetName().Contains(TEXT("Face")))
			{
				Visage = M;
				break;
			}
		}
	}

	if (!Visage)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Avatars : aucun maillage 'Face' sur %s — AnimBP facial non pose"),
			*Avatar->GetName());
		return;
	}

	// Chargement synchrone assume : on est au spawn, l'avatar doit etre
	// complet avant que le visiteur ne le voie.
	if (UClass* ClasseAnim = AnimBPFacial.LoadSynchronous())
	{
		Visage->SetAnimInstanceClass(ClasseAnim);
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Avatars : AnimBP facial = %s"), *ClasseAnim->GetName());
	}
	else
	{
		UE_LOG(LogGardeFrontiere, Error,
			TEXT("Avatars : AnimBP facial introuvable (%s)"),
			*AnimBPFacial.ToString());
	}
}

USkeletalMeshComponent* UAvatarSwitcherComponent::TrouverMaillageCorps() const
{
	if (!AvatarCourant)
	{
		return nullptr;
	}

	TArray<USkeletalMeshComponent*> Maillages;
	AvatarCourant->GetComponents<USkeletalMeshComponent>(Maillages);

	for (USkeletalMeshComponent* M : Maillages)
	{
		if (M && M->GetName().Contains(TEXT("Body")))
		{
			return M;
		}
	}

	// A defaut le premier : sur un MetaHuman, c'est la racine squelettique
	// dont le visage et les grooms suivent la pose.
	return Maillages.Num() > 0 ? Maillages[0] : nullptr;
}

void UAvatarSwitcherComponent::JouerAnimationCorps(UAnimationAsset* Animation, bool bBoucler)
{
	USkeletalMeshComponent* Corps = TrouverMaillageCorps();
	if (!Corps)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Avatars : aucun maillage de corps — posture ignoree"));
		return;
	}

	if (!Animation)
	{
		// Rendre la main a l'AnimBP plutot que de laisser l'agent fige sur
		// la derniere pose. Le visage et les grooms suivent le corps : le
		// figer les figerait tous.
		Corps->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		return;
	}

	Corps->PlayAnimation(Animation, bBoucler);
}

USkeletalMeshComponent* UAvatarSwitcherComponent::TrouverMaillageFacial() const
{
	if (!AvatarCourant)
	{
		return nullptr;
	}

	// Un MetaHuman porte plusieurs maillages skeletaux — corps, visage,
	// cheveux. Celui du visage est reconnaissable a son nom.
	TArray<USkeletalMeshComponent*> Maillages;
	AvatarCourant->GetComponents<USkeletalMeshComponent>(Maillages);

	for (USkeletalMeshComponent* M : Maillages)
	{
		if (M && M->GetName().Contains(TEXT("Face")))
		{
			return M;
		}
	}

	// A defaut, le premier : mieux vaut une expression sur le mauvais
	// maillage qu'aucune, et le log dira ou chercher.
	if (Maillages.Num() > 0)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Avatars : aucun maillage nomme 'Face', repli sur %s"),
			*Maillages[0]->GetName());
		return Maillages[0];
	}
	return nullptr;
}
