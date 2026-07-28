// Effet de glitch — masque la substitution d'avatar.
//
// Son role n'est pas decoratif : il couvre l'instant ou le MetaHuman est
// detruit puis respawne, ce qui transforme une substitution technique en
// effet voulu.
//
// Deux leviers distincts, releves dans l'ancien Blueprint :
//
//   Add or Update Blendable (In Weight 1.0 / 0.0)
//       branche ou debranche le materiau sur le PostProcessVolume
//   Set Scalar Parameter Value ("weight")
//       anime l'intensite pendant l'effet
//
// Couper l'un sans l'autre laisse soit un effet fige, soit un materiau
// inutilement actif sur le volume.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GlitchComponent.generated.h"

class APostProcessVolume;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USoundBase;
class UCurveFloat;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGlitchTermine);

UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API UGlitchComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGlitchComponent();

	/**
	 * Volume sur lequel appliquer l'effet.
	 *
	 * L'ancien Blueprint prenait le PREMIER renvoye par
	 * Get All Actors Of Class — or il y en a cinq dans Studio.umap et
	 * l'ordre n'est pas garanti. On le designe explicitement ; laisse vide
	 * pour retomber sur l'ancien comportement.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch")
	TObjectPtr<APostProcessVolume> VolumeCible;

	/** PPM_GLITCH. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch")
	TObjectPtr<UMaterialInterface> MateriauGlitch;

	/** Nom du parametre scalaire anime. "weight" dans PPM_GLITCH. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch")
	FName ParametreIntensite = TEXT("weight");

	/** Glitch_Sound_By_DyBoy. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch")
	TObjectPtr<USoundBase> SonGlitch;

	/**
	 * Profil d'intensite dans le temps. Sans courbe, un aller-retour
	 * triangulaire est applique.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch")
	TObjectPtr<UCurveFloat> CourbeIntensite;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch",
		meta = (ClampMin = "0.05", Units = "s"))
	float Duree = 1.2f;

	/** Retard avant le declenchement — 0,2 s dans l'ancien Blueprint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Glitch",
		meta = (ClampMin = "0.0", Units = "s"))
	float RetardAvantEffet = 0.2f;

	UPROPERTY(BlueprintReadOnly, Category = "Glitch")
	bool bEnCours = false;

	/**
	 * Effet en cours, retard d'amorcage compris.
	 *
	 * bEnCours seul ne suffit pas : pendant RetardAvantEffet la substitution
	 * est engagee mais l'effet n'a pas commence, et un appelant qui se fierait
	 * au seul drapeau croirait la voie libre.
	 */
	UFUNCTION(BlueprintPure, Category = "Glitch")
	bool EstEnCours() const;

	/** Emis a la fin — c'est le moment ou la substitution d'avatar est sure. */
	UPROPERTY(BlueprintAssignable, Category = "Glitch")
	FOnGlitchTermine OnGlitchTermine;

	UFUNCTION(BlueprintCallable, Category = "Glitch")
	void Declencher();

	UFUNCTION(BlueprintCallable, Category = "Glitch")
	void Arreter();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool PreparerMateriau();
	void Demarrer();
	void AppliquerIntensite(float Valeur);
	void BrancherSurVolume(float Poids);

	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> MateriauDynamique;
	UPROPERTY() TObjectPtr<APostProcessVolume> VolumeResolu;

	FTimerHandle MinuterieRetard;
	float Ecoule = 0.f;
};
