// Tampons : accepte, refuse, et panneau de sortie de zone.
//
// Reprend le mecanisme releve : le widget est cree UNE FOIS, ajoute au
// viewport, puis montre et masque par son opacite. Ni creation ni
// destruction a chaque session — moins d'allocations, et l'etat interne
// du widget survit.
//
// Le widget WBP_Stamp expose deux evenements personnalises :
//     ShowStamp(Decision : String)
//     ShowExitStamp()
// On les appelle par nom, faute de pouvoir referencer une classe Blueprint
// depuis le C++.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GuardSessionTypes.h"
#include "StampComponent.generated.h"

class UUserWidget;

UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API UStampComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStampComponent();

	/** WBP_Stamp. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tampon")
	TSubclassOf<UUserWidget> ClasseWidget;

	/**
	 * Noms des evenements a appeler sur le widget.
	 *
	 * Exposes plutot que codes en dur : si le widget est retouche, on
	 * corrige ici sans recompiler.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tampon|Interface")
	FName EvenementTampon = TEXT("ShowStamp");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tampon|Interface")
	FName EvenementSortie = TEXT("ShowExitStamp");

	/** Chaines attendues par ShowStamp, testees par un Contains cote widget. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tampon|Interface")
	FString DecisionAcceptee = TEXT("ACCEPTE");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tampon|Interface")
	FString DecisionRefusee = TEXT("REFUSE");

	UPROPERTY(BlueprintReadOnly, Category = "Tampon")
	TObjectPtr<UUserWidget> Widget;

	UFUNCTION(BlueprintCallable, Category = "Tampon")
	void AfficherVerdict(EGuardVerdict Decision);

	UFUNCTION(BlueprintCallable, Category = "Tampon")
	void AfficherSortieZone();

	/** Masque sans detruire — retour en veille. */
	UFUNCTION(BlueprintCallable, Category = "Tampon")
	void Masquer();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;

private:
	bool PreparerWidget();
	void AppelerEvenement(FName Nom, const FString& Argument = FString());
	void DefinirOpacite(float Opacite);
};
