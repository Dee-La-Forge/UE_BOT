// Pont WebSocket vers le sidecar IA local.
//
// Implemente le contrat decrit dans docs/CONTRAT-EVENEMENTS.md.
//
// Le sidecar ignore tout de la scenographie : il emet des faits, Unreal
// decide de la mise en scene. On peut ainsi retoucher glitch, tampons et
// panneau de sortie sans toucher a l'IA, et changer de LLM sans rouvrir
// un Blueprint.
//
// Regle de conduite : une panne du sidecar ne doit JAMAIS figer la borne.
// Toute perte de connexion remonte par OnPanne, et l'appelant bascule sur
// ses repliques de secours.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GuardSessionTypes.h"
#include "SidecarClient.generated.h"

class IWebSocket;

/** L'agent commence a parler. Le texte sert au sous-titrage eventuel. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnParoleDebut, const FString&, Texte, EGuardEmotion, Emotion);

/** L'agent a fini sa replique. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnParoleFin);

/** Verdict rendu : declenche le tampon accepte/refuse. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnVerdict, EGuardVerdict, Decision);

/** Entretien termine : declenche le panneau "quittez la zone". */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSessionTerminee);

/** Emotion definitive, une fois la replique complete. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnEmotion, EGuardEmotion, Emotion);

/** Le sidecar est injoignable : passer en mode degrade. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnPanne, const FString&, Raison);

/** Trame audio recue (PCM16 mono), a jouer et a relayer au lipsync. */
DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnAudioRecu, const TArray<uint8>& /*PCM16*/, int32 /*Taux*/);


UCLASS(BlueprintType)
class GARDEFRONTIERE_API USidecarClient : public UObject
{
	GENERATED_BODY()

public:
	/** ws://127.0.0.1:8765 par defaut — voir DefaultGame.ini. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Sidecar")
	void Connecter(const FString& Url);

	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Sidecar")
	void Deconnecter();

	UFUNCTION(BlueprintPure, Category = "Garde Frontiere|Sidecar")
	bool EstConnecte() const;

	// -- Emissions vers le sidecar ------------------------------------

	/** Un visiteur vient d'entrer dans le champ du capteur. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Sidecar")
	void SignalerPresence();

	/** Le visiteur a quitte la zone : remet la session a zero. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Sidecar")
	void SignalerAbsence();

	/** Remise a zero explicite (abandon, timeout, maintenance). */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Sidecar")
	void ReinitialiserSession();

	/**
	 * Envoie un segment de parole du visiteur.
	 *
	 * PCM16 mono 16 kHz — ce qu'attend Whisper. Le segment doit deja etre
	 * borne par le VAD (SileroVAD) : on ne transmet pas de silence.
	 */
	void EnvoyerAudioVisiteur(const TArray<uint8>& PCM16);

	// -- Evenements ----------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Sidecar")
	FOnParoleDebut OnParoleDebut;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Sidecar")
	FOnParoleFin OnParoleFin;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Sidecar")
	FOnVerdict OnVerdict;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Sidecar")
	FOnSessionTerminee OnSessionTerminee;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Sidecar")
	FOnEmotion OnEmotion;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Sidecar")
	FOnPanne OnPanne;

	/** Non expose au Blueprint : trames binaires, trop volumineuses. */
	FOnAudioRecu OnAudioRecu;

	virtual void BeginDestroy() override;

private:
	void EnvoyerEvenement(const FString& Nom);
	void TraiterMessage(const FString& Message);
	void TraiterBinaire(const TArray<uint8>& Donnees);

	void SurConnexion();
	void SurErreur(const FString& Erreur);
	void SurFermeture(int32 Code, const FString& Raison, bool bParPair);

	static EGuardEmotion VersEmotion(const FString& Valeur);
	static EGuardVerdict VersVerdict(const FString& Valeur);

	TSharedPtr<IWebSocket> Socket;

	/**
	 * Taux d'echantillonnage annonce par le dernier descripteur `parole.audio`.
	 *
	 * Le JSON precede toujours la trame binaire qu'il decrit : on retient
	 * donc ses metadonnees pour les appliquer aux octets qui suivent.
	 * Piper sort du 22050 Hz, pas du 24000 — d'ou la lecture dynamique
	 * plutot qu'une constante.
	 */
	int32 TauxAudioAttendu = 22050;
};
