// Chef d'orchestre de la borne.
//
// Remplace le "God Blueprint" BP_ConvaiCharacterBase de l'ancien projet, qui
// cumulait orchestration IA, selection d'avatar, gating micro, post-process,
// widget tampon, etat LiDAR, cycle de session et timers dans un seul asset
// binaire — indiffable, intestable, irrelisible.
//
// Ici la machine a etats est explicite et en C++ ; la scenographie reste en
// Blueprint, ou elle est a sa place. Les evenements ci-dessous sont les
// points d'accroche : glitch, changement d'avatar, tampons, panneau de
// sortie s'y branchent sans que ce fichier sache ce qu'ils sont.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GuardSessionTypes.h"
#include "Templates/PimplPtr.h"
#include "GuardSessionManager.generated.h"

class UAnimationAsset;
class USidecarClient;
class ULidarPresenceComponent;
class UAgentVoiceComponent;
class UAgentFaceComponent;
class UGlitchComponent;
class UAvatarSwitcherComponent;
class UStampComponent;
class UVisitorMicComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnPhaseChangee, EGuardPhase, NouvellePhase);

/** Un visiteur se presente : le Blueprint declenche glitch + switch avatar. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnSessionDemarree, int32, IndexAvatar);

/** Verdict rendu : le Blueprint affiche stamp_accepted ou stamp_refused. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnVerdictRendu, EGuardVerdict, Decision);

/** Le Blueprint affiche le panneau "quittez la zone". */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDemandeSortieZone);

/** Retour en veille : le Blueprint efface tampons et panneau. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnSessionFinie, EGuardFinDeSession, Raison);

/** Pose faciale a appliquer, via l'enum E_Emotions du plugin Convai. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnEmotionChangee, EGuardEmotion, Emotion);

/** Repli parle quand l'IA est indisponible — la borne ne reste jamais muette. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnRepliqueDeSecours, const FString&, Texte);

/** L'agent se met en posture d'ecoute, ou revient en garde. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEcouteChangee, bool, bEcoute);


UCLASS(Blueprintable, BlueprintType)
class GARDEFRONTIERE_API AGuardSessionManager : public AActor
{
	GENERATED_BODY()

public:
	AGuardSessionManager();


	// =====================================================================
	// Configuration — tout ce qui etait code en dur dans l'ancien projet
	// =====================================================================

	// =====================================================================
	// Interrupteurs de diagnostic
	//
	// Deux sous-systemes touchent a des ressources exterieures au moteur :
	// un port serie et une socket reseau. Les desactiver separement permet
	// d'isoler un blocage en deux essais, au lieu de le deviner.
	// =====================================================================

	/** Decocher pour demarrer sans ouvrir le port serie du capteur. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Diagnostic")
	bool bActiverCapteur = true;

	/** Decocher pour demarrer sans se connecter au sidecar. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Diagnostic")
	bool bActiverSidecar = true;

	/**
	 * Affiche la phase courante a l'ecran, en jeu.
	 *
	 * Le journal ne se lit qu'apres coup. Debout devant la borne, on ne sait
	 * pas distinguer une detection qui n'aboutit pas d'une detection qui n'a
	 * pas eu lieu — les deux se ressemblent trait pour trait.
	 *
	 * A decocher avant la mise en service.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Diagnostic")
	bool bAfficherEtatEcran = true;

	/** Adresse du sidecar IA. Modifiable sans recompiler. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Sidecar")
	FString UrlSidecar = TEXT("ws://127.0.0.1:8765");

	/**
	 * Fournisseur Audio2Face. Laisser vide pour prendre le local disponible.
	 *
	 * Le defaut du plugin est "RemoteA2F", qui tente de joindre le cloud
	 * NVIDIA et echoue au bout de dix secondes. Une borne d'exposition n'a
	 * pas de reseau et n'en veut pas : on cherche donc un fournisseur dont
	 * le nom commence par "LocalA2F" — les modeles s'enregistrent ainsi,
	 * "LocalA2F-James" pour celui-ci, et seulement s'ils ont pu charger
	 * leurs poids. Leur presence vaut donc test de disponibilite.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Audio2Face")
	FName FournisseurA2F;

	/**
	 * Joue aussi la voix par AgentVoiceComponent, en plus d'Audio2Face.
	 *
	 * DESACTIVE. Le composant ACE joue lui-meme ce qu'il renvoie : doubler
	 * faisait parler l'agent deux fois. Ce drapeau n'a servi qu'a etablir que
	 * l'animation faciale fonctionnait pendant que la lecture d'ACE paraissait
	 * muette — elle ne l'etait pas, elle etait couverte.
	 *
	 * Conserve comme repli de mise au point, si la lecture d'ACE venait a
	 * defaillir sur une autre machine.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Audio2Face")
	bool bDoublerVoixPourDiagnostic = false;

	// La rotation d'avatars appartient a UAvatarSwitcherComponent :
	// selectionne le composant "Avatars" pour regler ClassesAvatars,
	// TransformSpawn et l'anti-repetition.

	/**
	 * Poste un garde dans la guerite des le lancement, sans attendre de
	 * visiteur.
	 *
	 * Sans cela la scene reste vide en veille : l'avatar n'apparait qu'au
	 * demarrage d'une session, et l'installation montre une guerite deserte
	 * tant que personne ne s'est presente.
	 *
	 * Effet de bord souhaitable : le garde en veille compte comme avatar
	 * precedent, donc le premier visiteur en voit forcement un autre — le
	 * glitch masque alors une vraie substitution, et non une apparition.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Scenographie")
	bool bAvatarEnVeille = true;

	/** Delai sans reponse du visiteur avant relance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Delais",
		meta = (ClampMin = "1.0", Units = "s"))
	float DelaiReponseVisiteur = 12.f;

	/** Delai total sans interaction avant abandon de la session. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Delais",
		meta = (ClampMin = "5.0", Units = "s"))
	float DelaiAbandon = 30.f;

	/**
	 * Delai apres le verdict avant d'inviter le visiteur a sortir.
	 * Laisse le temps de lire le tampon.
	 *
	 * 7 s : valeur relevee dans l'ancien Blueprint
	 * (Set Timer by Function Name "SwitchToExitStamp", Time = 7.0).
	 * J'avais suppose 4 s.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Delais",
		meta = (ClampMin = "0.0", Units = "s"))
	float DelaiAvantSortie = 7.f;

	/** Delai de reconnexion au sidecar apres une panne. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Sidecar",
		meta = (ClampMin = "1.0", Units = "s"))
	float DelaiReconnexion = 5.f;

	// =====================================================================
	// Etat
	// =====================================================================

	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere|Etat")
	EGuardPhase Phase = EGuardPhase::Veille;

	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere|Etat")
	EGuardVerdict DernierVerdict = EGuardVerdict::EnCours;

	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere|Etat")
	int32 IndexAvatarCourant = 0;

	/** Vrai quand le sidecar repond ; faux = mode degrade. */
	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere|Etat")
	bool bIADisponible = false;

	/**
	 * Vrai tant que la parole du visiteur a un sens : accueil et
	 * interrogatoire, jamais apres le verdict.
	 *
	 * C'est la frontiere qui manquait. Une fois le verdict rendu, tout enonce
	 * encore transmis faisait produire au sidecar un nouveau verdict, qui
	 * relancait le cycle — la borne oscillait entre Verdict et SortieZone
	 * aussi longtemps que le visiteur parlait.
	 */
	UFUNCTION(BlueprintPure, Category = "Garde Frontiere|Etat")
	bool ConversationEnCours() const;

	// =====================================================================
	// Evenements — points d'accroche de la scenographie
	// =====================================================================

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnPhaseChangee OnPhaseChangee;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnSessionDemarree OnSessionDemarree;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnVerdictRendu OnVerdictRendu;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnDemandeSortieZone OnDemandeSortieZone;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnSessionFinie OnSessionFinie;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnEmotionChangee OnEmotionChangee;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnRepliqueDeSecours OnRepliqueDeSecours;

	UPROPERTY(BlueprintAssignable, Category = "Garde Frontiere|Evenements")
	FOnEcouteChangee OnEcouteChangee;

	// =====================================================================
	// Postures
	// =====================================================================

	/**
	 * Animation jouee quand un visiteur se presente.
	 *
	 * Une pose statique suffit — l'agent se tient face au poste, tete et yeux
	 * droits. Aucun suivi du regard : le LiDAR ne rend qu'une distance,
	 * jamais un angle, et l'agent ne saurait donc pas ou tourner la tete.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Postures")
	TObjectPtr<UAnimationAsset> AnimationEcoute;

	/** Animation de retour en veille. Laisser vide pour rendre la main a l'AnimBP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Postures")
	TObjectPtr<UAnimationAsset> AnimationVeille;

	/** Vrai des qu'une session est ouverte. */
	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere|Etat")
	bool bEnEcoute = false;

	// =====================================================================
	// Pilotage manuel — exploitation et mise au point
	// =====================================================================

	/** Force le demarrage d'une session, capteur ou non. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Pilotage")
	void DemarrerSession();

	/** Interrompt et remet en veille. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Pilotage")
	void TerminerSession(EGuardFinDeSession Raison);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<ULidarPresenceComponent> Presence;

	/** Joue les trames audio du sidecar au fil de leur arrivee. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<UAgentVoiceComponent> Voix;

	/** Ecrit les poids d'emotion sur l'AnimBP du MetaHuman actif. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<UAgentFaceComponent> Visage;

	/** Masque la substitution d'avatar. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<UGlitchComponent> Glitch;

	/** Detruit et respawne le MetaHuman entre deux visiteurs. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<UAvatarSwitcherComponent> Avatars;

	/** Tampons accepte/refuse et panneau de sortie. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<UStampComponent> Tampons;

	/** Capte la parole du visiteur et la borne — le pendant de Voix. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<UVisitorMicComponent> Micro;

	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere")
	TObjectPtr<USidecarClient> Sidecar;

	/**
	 * Transmet un segment de parole du visiteur au sidecar.
	 *
	 * A appeler depuis le Blueprint, avec les echantillons que SileroVAD
	 * vient de borner. La conversion vers PCM16 16 kHz mono — ce qu'attend
	 * Whisper — est faite ici : le Blueprint n'a pas a s'en soucier.
	 */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Audio")
	void TransmettreParoleVisiteur(const TArray<float>& Echantillons,
		int32 TauxSource, int32 NbCanaux = 1);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;

private:
	void ChangerPhase(EGuardPhase Nouvelle);

	/** Pose l'agent selon bEnEcoute. Rejouee a chaque substitution d'avatar. */
	void AppliquerPosture();

	/**
	 * Ouvre la scene : diffuse l'avatar en place, puis lance le dialogue.
	 *
	 * Appelee APRES la substitution, jamais avant — l'index diffus doit etre
	 * celui de l'avatar reellement present, et l'agent qui parle doit exister.
	 */
	void OuvrirLaScene();

	// -- Audio2Face -------------------------------------------------------

	/** Ouvre une session A2F sur l'avatar courant. Rend vrai si elle est prete. */
	bool OuvrirSessionA2F(int32 Taux);

	/** Clot le flux : Audio2Face finit de jouer ce qu'il a deja recu. */
	void FermerSessionA2F();

	/**
	 * Session Audio2Face de la replique en cours.
	 *
	 * Une par replique. La session porte le taux d'echantillonnage et l'etat
	 * du flux, et se termine sur bEndOfSamples — apres quoi elle n'accepte
	 * plus rien. En rouvrir une a chaque replique coute moins cher que de
	 * maintenir un flux perpetuel qui devrait distinguer les silences entre
	 * deux phrases des fins de replique.
	 *
	 * TPimplPtr et non TUniquePtr : FAudio2XSession n'est ici que declare en
	 * avant, et le code genere par UHT detruirait un type incomplet. TPimplPtr
	 * capture le destructeur a la construction, la ou le type est complet.
	 */
	TPimplPtr<class FAudio2XSession> SessionA2F;

	/** Taux de la session ouverte, pour la rouvrir si le sidecar en changeait. */
	int32 TauxSessionA2F = 0;

	/**
	 * Vrai entre parole.debut et parole.fin.
	 *
	 * Seule cette fenetre autorise l'ouverture d'une session A2F. Une trame
	 * arrivant en dehors est un reliquat de la replique qui vient de se
	 * clore : lui ouvrir une session la ferait jouer par-dessus celle qui
	 * s'entend encore, et le visiteur entendrait deux voix.
	 */
	bool bRepliqueEnCours = false;

	// Reactions au capteur de presence
	UFUNCTION() void SurPresenceDetectee();
	UFUNCTION() void SurPresencePerdue();

	// Reactions a la scenographie
	UFUNCTION() void SurGlitchTermine();
	UFUNCTION() void SurAvatarChange(AActor* NouvelAvatar, int32 Index);

	// Reactions au sidecar
	UFUNCTION() void SurParoleDebut(const FString& Texte, EGuardEmotion Emotion);
	UFUNCTION() void SurParoleFin();
	UFUNCTION() void SurVerdict(EGuardVerdict Decision);
	UFUNCTION() void SurSessionTerminee();
	UFUNCTION() void SurPanneIA(const FString& Raison);

	// Minuteries
	void ArmerAbandon();
	void AnnulerMinuteries();
	UFUNCTION() void SurAbandon();
	UFUNCTION() void SurDelaiSortie();
	UFUNCTION() void TenterReconnexion();

	FTimerHandle MinuterieAbandon;
	FTimerHandle MinuterieSortie;
	FTimerHandle MinuterieReconnexion;

	/** Distincte des cles du capteur, pour que les deux messages coexistent. */
	static constexpr uint64 CleAffichagePhase = 8810;
};
