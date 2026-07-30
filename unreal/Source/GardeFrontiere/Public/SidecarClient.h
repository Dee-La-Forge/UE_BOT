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

/**
 * Trame audio recue (PCM16 mono), a jouer telle quelle.
 *
 * Le lipsync ne passe pas par ici : il viendra d'Audio2Face via LiveLink,
 * qui alimente l'AnimBP facial sans transiter par le C++.
 */
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

	/**
	 * Le visiteur n'a rien dit depuis DelaiReponseVisiteur : l'agent le
	 * relance. Le sidecar traite ce silence comme un tour de parole.
	 */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Sidecar")
	void SignalerSilence();

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
	 * Une tentative de connexion est partie et ne s'est pas encore resolue.
	 *
	 * IsConnected() est faux pendant tout le handshake : sans ce drapeau, la
	 * minuterie de reconnexion remplacait un socket en cours de connexion,
	 * dont les delegues restaient vivants — double connexion si l'ancien
	 * aboutissait, acces a un objet detruit si une trame tardive arrivait.
	 */
	bool bConnexionEnCours = false;

	/**
	 * Taux d'echantillonnage annonce par le dernier descripteur `parole.audio`.
	 *
	 * Le JSON precede toujours la trame binaire qu'il decrit : on retient
	 * donc ses metadonnees pour les appliquer aux octets qui suivent.
	 * Piper sort du 22050 Hz, pas du 24000 — d'ou la lecture dynamique
	 * plutot qu'une constante.
	 */
	int32 TauxAudioAttendu = 22050;

	/**
	 * Message binaire en cours de reassemblage.
	 *
	 * OnRawMessage livre par fragments : on accumule jusqu'a ce que le
	 * parametre `Restant` retombe a zero, sans quoi une frontiere sur un
	 * octet impair decalerait tout le PCM qui suit.
	 */
	TArray<uint8> FragmentEnCours;

	/**
	 * Un descripteur `parole.audio` a ete recu : la prochaine trame est de
	 * l'audio, et elle seule.
	 *
	 * L'implementation WebSocket d'Unreal presente AUSSI les messages
	 * TEXTE a OnRawMessage. Sans ce drapeau, chaque JSON du contrat
	 * repartait dans le chemin audio — releve le 30/07/2026 : les
	 * 62 octets de {"evenement": "session.demarree", ...} y sont arrives
	 * une milliseconde apres avoir ete lus comme JSON. En pleine replique,
	 * le descripteur se serait injecte dans le PCM entre deux morceaux de
	 * voix.
	 */
	bool bTrameAudioAttendue = false;

	/**
	 * Un `parole.fin` est arrive alors qu'une trame audio annoncee n'avait
	 * pas encore fini d'etre reassemblee : il attend son tour.
	 *
	 * Les deux canaux d'Unreal ne vont pas a la meme vitesse. Un message
	 * TEXTE passe par OnMessage et part aussitot ; un message BINAIRE
	 * passe par OnRawMessage, qui le livre par fragments qu'il faut
	 * recoller. Quand le sidecar envoie sa derniere trame et clot dans la
	 * foulee, le `parole.fin` double la trame en vol.
	 *
	 * Consequence mesuree le 31/07/2026, sonde a l'appui : parole.fin
	 * arrivait UNE milliseconde apres le binaire, remettait
	 * bRepliqueEnCours a faux, et la trame achevait son reassemblage pour
	 * se faire jeter — « Audio rejete : ... hors replique ». La derniere
	 * phrase de l'agent n'etait jamais prononcee.
	 *
	 * Le defaut existait avant l'intro recitee ; il ne se voyait pas parce
	 * que la generation du modele laissait toujours un long silence entre
	 * la derniere trame et la cloture. Une intro qui n'attend rien l'a mis
	 * au jour — les 90 ms entre deux phrases suffisaient, la milliseconde
	 * de la cloture non.
	 *
	 * On differe donc la cloture jusqu'a ce que la trame annoncee soit
	 * remise. Le drapeau est purge a la deconnexion : sans cela, un cable
	 * arrache entre le descripteur et sa trame laisserait bRepliqueEnCours
	 * verrouille a vrai — micro sourd pour le reste de la session.
	 */
	bool bFinDiffere = false;
};
