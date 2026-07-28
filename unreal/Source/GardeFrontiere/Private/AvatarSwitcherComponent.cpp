#include "AvatarSwitcherComponent.h"

#include "GardeFrontiere.h"
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

	AvatarCourant = Monde->SpawnActor<AActor>(
		ClassesAvatars[Index], TransformSpawn, Params);

	if (!AvatarCourant)
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Avatars : echec du spawn (index %d)"), Index);
		return nullptr;
	}

	IndexCourant = Index;
	UE_LOG(LogGardeFrontiere, Log, TEXT("Avatars : %s en place (index %d)"),
		*AvatarCourant->GetName(), Index);

	OnAvatarChange.Broadcast(AvatarCourant, Index);
	return AvatarCourant;
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
