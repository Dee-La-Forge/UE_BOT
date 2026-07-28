#include "AvatarSwitcherComponent.h"

#include "GardeFrontiere.h"
#include "Algo/Find.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UAvatarSwitcherComponent::UAvatarSwitcherComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
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
	if (bRetirerConvaiConversationnel)
	{
		RetirerConvaiConversationnel(Nouveau);
	}

	AvatarCourant = Nouveau;

	IndexCourant = Index;
	UE_LOG(LogGardeFrontiere, Log, TEXT("Avatars : %s en place (index %d)"),
		*AvatarCourant->GetName(), Index);

	OnAvatarChange.Broadcast(AvatarCourant, Index);
	return AvatarCourant;
}

void UAvatarSwitcherComponent::RetirerConvaiConversationnel(AActor* Avatar) const
{
	if (!Avatar)
	{
		return;
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

			Composant->DestroyComponent();
			++NbRetires;
			break;
		}
	}

	// Un retrait qui ne retire rien doit se voir. La premiere version de ce
	// code cherchait les composants trop tot et n'en trouvait aucun — sans
	// rien dire, ce qui a coute un cycle de compilation et de test complet
	// pour un diagnostic que cette ligne aurait donne tout de suite.
	if (NbRetires == 0)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Avatars : aucun composant conversationnel Convai trouve sur %s ")
			TEXT("(%d composants inspectes) — le gel a l'arret vient d'ailleurs."),
			*Avatar->GetName(), Composants.Num());
	}
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
