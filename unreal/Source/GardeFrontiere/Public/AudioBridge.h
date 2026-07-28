// Conversions audio entre Unreal et le sidecar.
//
// Deux formats, et il ne faut pas les confondre :
//
//   visiteur -> sidecar : PCM16, 16 kHz, mono   (ce qu'attend Whisper)
//   sidecar  -> Unreal  : PCM16, 22,05 kHz mono (sortie de Piper)
//
// Le taux de sortie n'est PAS 24 kHz comme on pourrait le supposer : la voix
// fr_FR-siwis-medium echantillonne a 22050 Hz. Il est annonce dans chaque
// descripteur `parole.audio`, donc lu dynamiquement plutot que suppose.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AudioBridge.generated.h"

/** Taux attendu par le STT du sidecar. */
#define GF_TAUX_VISITEUR 16000

UCLASS()
class GARDEFRONTIERE_API UAudioBridge : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Convertit des echantillons flottants en PCM16 16 kHz mono,
	 * pret a etre envoye au sidecar.
	 *
	 * @param Echantillons  PCM flottant dans [-1, 1]
	 * @param TauxSource    taux d'echantillonnage des donnees fournies
	 * @param NbCanaux      1 ou 2 ; le stereo est replie en mono
	 */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Audio")
	static TArray<uint8> VersPCM16Visiteur(
		const TArray<float>& Echantillons, int32 TauxSource, int32 NbCanaux = 1);

	/** Convertit du PCM16 en echantillons flottants. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Audio")
	static TArray<float> DepuisPCM16(const TArray<uint8>& PCM16);

	/**
	 * Reechantillonne par interpolation lineaire.
	 *
	 * Suffisant ici : la qualite du micro et la robustesse de Whisper sont
	 * les facteurs limitants, pas l'ordre du filtre d'interpolation.
	 */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Audio")
	static TArray<float> Reechantillonner(
		const TArray<float>& Entree, int32 TauxSource, int32 TauxCible);

	/** Replie un signal entrelace en mono par moyenne des canaux. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Audio")
	static TArray<float> VersMono(const TArray<float>& Entrelace, int32 NbCanaux);

	/**
	 * Niveau crete d'un segment, dans [0, 1].
	 *
	 * Utile au diagnostic sur site : un micro debranche ou coupe donne 0,
	 * et l'exploitant le voit sans avoir a lire un journal.
	 */
	UFUNCTION(BlueprintPure, Category = "Garde Frontiere|Audio")
	static float NiveauCrete(const TArray<float>& Echantillons);
};
