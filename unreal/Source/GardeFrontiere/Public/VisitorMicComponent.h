// Acquisition de la parole du visiteur.
//
// Ce maillon manquait entierement. Le sidecar savait ecouter — STT sur GPU,
// machine a etats attendant cinq questions — mais rien ne lui envoyait jamais
// un echantillon : aucun Blueprint n'appelait TransmettreParoleVisiteur. La
// borne parlait sans entendre.
//
// L'ancien projet capturait bien le micro, mais par le ConvaiPlayerComponent,
// qui alimentait la pile Convai et non la notre.
//
// Tout est fait ici plutot qu'en Blueprint : la chaine capture -> mono ->
// reechantillonnage -> VAD -> segment est une suite d'operations sur des
// tableaux de flottants, illisible en noeuds et impossible a relire.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HAL/CriticalSection.h"
#include "VisitorMicComponent.generated.h"

class UCapturableSoundWave;
class URuntimeVADProviderBase;

/**
 * Un enonce complet du visiteur, borne par le VAD.
 *
 * Toujours en 16 kHz mono : c'est ce qu'attend Whisper, et convertir une
 * seule fois evite de trainer le format du peripherique dans tout le code.
 */
DECLARE_MULTICAST_DELEGATE_ThreeParams(
	FOnSegmentVisiteur, const TArray<float>& /*Echantillons*/,
	int32 /*Taux*/, int32 /*NbCanaux*/);


UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API UVisitorMicComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVisitorMicComponent();

	// -- Liaison ----------------------------------------------------------

	/** Decocher pour demarrer sans ouvrir le micro. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Micro|Diagnostic")
	bool bActiverMicro = true;

	/**
	 * Index du peripherique d'entree, dans l'ordre du systeme.
	 *
	 * 0 designe le peripherique par defaut de Windows, ce qui convient a une
	 * borne ou un seul micro est branche. Le journal enumere les autres au
	 * demarrage.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Micro|Liaison",
		meta = (ClampMin = "0"))
	int32 PeripheriqueMicro = 0;

	// -- Segmentation -----------------------------------------------------

	/**
	 * Silence necessaire pour clore un enonce.
	 *
	 * Trop court, on coupe le visiteur au milieu d'une phrase — les pauses
	 * naturelles atteignent 400 ms. Trop long, la borne parait lente a
	 * repondre. Le VAD tranche parole/silence ; ce delai tranche
	 * pause/fin d'enonce, ce qui n'est pas la meme decision.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Micro|Segmentation",
		meta = (ClampMin = "0.1", Units = "s"))
	float SilenceFinSegment = 0.75f;

	/**
	 * En deca, on n'envoie pas : Whisper INVENTE sur les fragments courts.
	 *
	 * Releve le 29/07/2026 : a 0.35 s, aucun fragment n'etait jamais rejete,
	 * et le sidecar recevait des demi-syllabes. Whisper rendait alors ses
	 * hallucinations d'entrainement — « Sous-titres realises par la
	 * communaute d'Amara.org » sur du quasi-silence, « Merci beaucoup » sur
	 * un souffle. L'agent enchainait ses questions sans rien comprendre, ce
	 * qui passait pour un defaut du modele alors que l'entree etait vide.
	 *
	 * Mieux vaut ignorer un debut de phrase que repondre a une invention.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Micro|Segmentation",
		meta = (ClampMin = "0.05", Units = "s"))
	float DureeMinSegment = 0.7f;

	/**
	 * Silence conserve en queue d'enonce avant transmission.
	 *
	 * A distinguer de SilenceFinSegment, avec lequel je les avais confondus.
	 * Celui-la decide QUAND l'enonce est fini — il doit rester genereux, sinon
	 * on coupe le visiteur au milieu d'une respiration. Celui-ci decide
	 * COMBIEN de ce silence on transmet, et il doit rester court.
	 *
	 * Les envoyer confondus faisait porter a chaque enonce 600 ms de vide :
	 * sur un enonce de 930 ms, deux tiers de silence, que Whisper transcrivait
	 * consciencieusement. Le temps de transcription variait de 192 a 672 ms
	 * selon la longueur — toute l'instabilite de la latence venait de la.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Micro|Segmentation",
		meta = (ClampMin = "0.0", ClampMax = "1.0", Units = "s"))
	float MargeSilenceTransmise = 0.25f;

	/**
	 * Au-dela, on coupe et on transmet.
	 *
	 * Un visiteur intarissable — ou un micro qui capte une conversation de
	 * salle — produirait sinon un segment sans fin, que le STT mettrait des
	 * dizaines de secondes a transcrire.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Micro|Segmentation",
		meta = (ClampMin = "1.0", Units = "s"))
	float DureeMaxSegment = 15.f;

	// -- Etat -------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Micro|Etat")
	bool bCaptureOuverte = false;

	UPROPERTY(BlueprintReadOnly, Category = "Micro|Etat")
	bool bEcoute = false;

	UPROPERTY(BlueprintReadOnly, Category = "Micro|Etat")
	bool bParoleEnCours = false;

	/** Enonce complet, pret a partir vers le sidecar. */
	FOnSegmentVisiteur OnSegmentVisiteur;

	/**
	 * Ouvre l'ecoute — le temps d'une session.
	 *
	 * La capture, elle, reste ouverte en permanence : ouvrir un peripherique
	 * audio coute des centaines de millisecondes, et les payer a l'arrivee de
	 * chaque visiteur ferait manquer ses premiers mots.
	 */
	UFUNCTION(BlueprintCallable, Category = "Micro")
	void DemarrerEcoute();

	/** Ferme l'ecoute et jette l'enonce en cours. */
	UFUNCTION(BlueprintCallable, Category = "Micro")
	void ArreterEcoute();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	void OuvrirCapture();
	void FermerCapture();
	void PreparerVAD();

	/** Clot l'enonce courant et le diffuse s'il est assez long. */
	void FermerSegment();

	UPROPERTY() TObjectPtr<UCapturableSoundWave> Micro;
	UPROPERTY() TObjectPtr<URuntimeVADProviderBase> VAD;

	/**
	 * Tampon rempli par le THREAD DE CAPTURE, vide par le thread de jeu.
	 *
	 * Le delegue du plugin se declenche hors du thread de jeu : on ne peut y
	 * toucher ni a un UObject ni au segment en cours. On se contente d'y
	 * empiler des flottants, sous verrou.
	 */
	FCriticalSection Verrou;
	TArray<float> Entrant;

	/**
	 * Format du peripherique, releve au PREMIER DRAIN — jamais a l'ouverture.
	 *
	 * GetSampleRate() rend 0 tant qu'aucun buffer n'a ete capte : le plugin
	 * ne renseigne le format qu'a l'arrivee du premier buffer, apres
	 * StartCapture. Zero vaut « pas encore connu ».
	 */
	int32 TauxCapture = 0;
	int32 CanauxCapture = 0;

	/** Enonce en construction, en 16 kHz mono. Thread de jeu uniquement. */
	TArray<float> Segment;
	float SilenceEcoule = 0.f;

	/** Reliquat entre deux trames, pour ne decouper le VAD que sur des frames pleines. */
	TArray<float> Reliquat;

	FDelegateHandle PoigneeAudio;

	/**
	 * Derniere purge du tampon interne du plugin.
	 *
	 * UStreamingSoundWave accumule TOUT l'audio capture, indefiniment, et sa
	 * documentation previent qu'il faut liberer a la main. Sur une borne qui
	 * tourne une journee, c'est une fuite garantie.
	 */
	double DerniereLiberation = 0.0;
};
