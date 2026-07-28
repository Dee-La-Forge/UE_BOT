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
	const int32 N = ClassesAvatars.Num();
	if (N <= 1)
	{
		return 0;
	}

	int32 Index = FMath::RandRange(0, N - 1);

	// Avec trois avatars, un tirage libre redonne le meme une fois sur
	// trois. On decale plutot que de retirer en boucle : temps constant,
	// et distribution uniforme sur les N-1 restants.
	if (bEviterRepetition && Index == IndexPrecedent)
	{
		Index = (IndexPrecedent + 1 + FMath::RandRange(0, N - 2)) % N;
	}

	IndexPrecedent = Index;
	return Index;
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
	if (ClassesAvatars.Num() == 0)
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Avatars : aucune classe configuree"));
		return nullptr;
	}
	return Spawner(Tirer());
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
