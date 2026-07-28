#include "StampComponent.h"

#include "GardeFrontiere.h"
#include "Blueprint/UserWidget.h"
#include "Kismet/GameplayStatics.h"

UStampComponent::UStampComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UStampComponent::BeginPlay()
{
	Super::BeginPlay();
	PreparerWidget();
}

void UStampComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	if (Widget)
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}
	Super::EndPlay(Raison);
}

bool UStampComponent::PreparerWidget()
{
	if (Widget)
	{
		return true;
	}
	if (!ClasseWidget)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Tampon : aucune classe de widget assignee (attendu : WBP_Stamp)"));
		return false;
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		return false;
	}

	Widget = CreateWidget<UUserWidget>(PC, ClasseWidget);
	if (!Widget)
	{
		return false;
	}

	// Cree une fois et laisse en viewport, masque par l'opacite.
	Widget->AddToViewport();
	DefinirOpacite(0.f);

	UE_LOG(LogGardeFrontiere, Log, TEXT("Tampon : widget pret"));
	return true;
}

void UStampComponent::DefinirOpacite(float Opacite)
{
	if (Widget)
	{
		Widget->SetRenderOpacity(Opacite);
	}
}

void UStampComponent::AppelerEvenement(FName Nom, const FString& Argument)
{
	if (!Widget)
	{
		return;
	}

	UFunction* Fonction = Widget->FindFunction(Nom);
	if (!Fonction)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Tampon : evenement '%s' introuvable sur %s"),
			*Nom.ToString(), *Widget->GetClass()->GetName());
		return;
	}

	if (Argument.IsEmpty())
	{
		Widget->ProcessEvent(Fonction, nullptr);
		return;
	}

	// L'evenement attend une chaine : on construit son tampon de parametres
	// depuis la signature, plutot que de supposer une disposition memoire.
	void* Params = FMemory_Alloca(Fonction->ParmsSize);
	FMemory::Memzero(Params, Fonction->ParmsSize);

	bool bRenseigne = false;
	for (TFieldIterator<FProperty> It(Fonction); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (FStrProperty* S = CastField<FStrProperty>(*It))
		{
			S->SetPropertyValue_InContainer(Params, Argument);
			bRenseigne = true;
			break;
		}
	}

	if (!bRenseigne)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Tampon : '%s' n'attend pas de chaine — appel sans argument"),
			*Nom.ToString());
	}

	Widget->ProcessEvent(Fonction, Params);
}

void UStampComponent::AfficherVerdict(EGuardVerdict Decision)
{
	if (!PreparerWidget())
	{
		return;
	}

	const FString Chaine = (Decision == EGuardVerdict::Accepte)
		? DecisionAcceptee
		: DecisionRefusee;

	DefinirOpacite(1.f);
	AppelerEvenement(EvenementTampon, Chaine);

	UE_LOG(LogGardeFrontiere, Log, TEXT("Tampon : %s"), *Chaine);
}

void UStampComponent::AfficherSortieZone()
{
	if (!PreparerWidget())
	{
		return;
	}

	DefinirOpacite(1.f);
	AppelerEvenement(EvenementSortie);

	UE_LOG(LogGardeFrontiere, Log, TEXT("Tampon : panneau de sortie de zone"));
}

void UStampComponent::Masquer()
{
	DefinirOpacite(0.f);
}
