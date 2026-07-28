// Voix de l'agent : joue les trames audio venues du sidecar.
//
// Le sidecar n'envoie pas un fichier mais un FLUX : la premiere phrase part
// des qu'elle est synthetisee, pendant que le LLM ecrit la suite. C'est de
// la que vient le gain de latence — 694 ms mesures jusqu'au premier son.
//
// Il faut donc une file d'attente, pas une lecture de son ponctuelle :
// USoundWaveProcedural, alimentee au fil de l'eau.

#pragma once

#include "CoreMinimal.h"
#include "Components/AudioComponent.h"
#include "AgentVoiceComponent.generated.h"

class USoundWaveProcedural;

/** L'agent commence effectivement a emettre du son. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnVoixDemarree);

/** La file est vide : l'agent a fini de parler. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnVoixTerminee);

UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API UAgentVoiceComponent : public UAudioComponent
{
	GENERATED_BODY()

public:
	UAgentVoiceComponent();

	/**
	 * Ajoute une trame a la file de lecture.
	 *
	 * @param PCM16  echantillons signes 16 bits, mono
	 * @param Taux   taux d'echantillonnage annonce par le sidecar
	 *
	 * Le premier appel demarre la lecture. Les suivants s'enchainent sans
	 * coupure tant que la file ne se vide pas.
	 */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Voix")
	void EmpilerTrame(const TArray<uint8>& PCM16, int32 Taux);

	/** Coupe la parole et vide la file — abandon, reset, panne. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Voix")
	void Interrompre();

	UFUNCTION(BlueprintPure, Category = "Garde Frontiere|Voix")
	bool EstEnTrainDeParler() const { return bParle; }

	/** Octets encore en attente de lecture. */
	UFUNCTION(BlueprintPure, Category = "Garde Frontiere|Voix")
	int32 OctetsEnAttente() const;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Voix")
	FOnVoixDemarree OnVoixDemarree;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Voix")
	FOnVoixTerminee OnVoixTerminee;

	/**
	 * Silence tolere avant de declarer la fin de parole.
	 *
	 * Sans ce delai, un creux entre deux phrases du flux serait pris pour
	 * une fin de replique, et le visage repasserait a l'ecoute en plein
	 * milieu d'une phrase.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Voix",
		meta = (ClampMin = "0.05", Units = "s"))
	float DelaiFinDeParole = 0.35f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void PreparerFlux(int32 Taux);

	UPROPERTY() TObjectPtr<USoundWaveProcedural> Flux;

	bool bParle = false;
	int32 TauxCourant = 0;
	float SilenceEcoule = 0.f;
};
