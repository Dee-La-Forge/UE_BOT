// Extension locale du plugin RuntimeAudioImporter — PAS un fichier d'origine.
//
// Base des fournisseurs de detection d'activite vocale (VAD). L'original de
// cette classe a ete perdu avec l'installation de juillet 2026 ; ce fichier
// la RECONSTRUIT a l'identique depuis ses deux consommateurs, qui en fixent
// le contrat exact :
//
//   - RuntimeAudioImporterSileroVAD (plugin du projet) : la signature de
//     chaque override, RuntimeSileroVADProvider.h ;
//   - UVisitorMicComponent (module GardeFrontiere) : la semantique de
//     ProcessAudio — negatif = erreur du fournisseur (trame ignoree),
//     0 = silence, positif = parole en cours.
//
// A re-appliquer si le plugin RuntimeAudioImporter est reinstalle depuis
// une source vierge : sans cette classe, ni le plugin Silero ni
// VisitorMicComponent ne compilent.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RuntimeVADProviderBase.generated.h"

/**
 * Base class for Voice Activity Detection providers.
 *
 * Concrete providers (e.g. Silero) subclass this and are instantiated by
 * class path, so that consumers compile without the provider plugin.
 */
UCLASS(Abstract, BlueprintType, Category = "Voice Activity Detector")
class RUNTIMEAUDIOIMPORTER_API URuntimeVADProviderBase : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Reset the provider's internal state (start of a new listening window).
	 * @return True if the provider is ready to process audio
	 */
	virtual bool Reset() { return false; }

	/**
	 * Feed one frame of mono PCM audio and get the current voice decision.
	 * @param PCMData Mono float32 samples in [-1, 1]
	 * @param SampleRate Sample rate of PCMData, in Hz
	 * @return Negative on provider error (frame should be ignored),
	 *         0 when no speech is active, positive while speech is active
	 */
	virtual int32 ProcessAudio(const TArray<float>& PCMData, int32 SampleRate) { return -1; }

	/** Sample rate the provider expects, in Hz. */
	virtual int32 GetRequiredSampleRate() const { return 16000; }

	/** Duration of one analysis frame, in milliseconds. */
	virtual float GetFrameDurationMs() const { return 32.0f; }

	/** Whether speech is currently considered active. */
	virtual bool IsSpeechActive() const { return false; }

	/** Called by the provider when a speech segment starts. */
	virtual void OnSpeechStarted() {}

	/** Called by the provider when a speech segment ends. */
	virtual void OnSpeechEnded() {}
};
