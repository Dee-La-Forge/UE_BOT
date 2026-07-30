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
	 * Inference en rafale plutot qu'en temps reel.
	 *
	 * Le mode par defaut d'ACE BRIDE l'inference a la vitesse de lecture :
	 * il ne va jamais plus vite que l'audio ne dure. Mesure a l'appui, une
	 * replique de 3,8 s mettait 3,47 s a etre animee AVANT de commencer a
	 * s'entendre — et la latence suivait la longueur de la replique, d'ou
	 * l'irregularite ressentie.
	 *
	 *    32 trames ->   686 ms
	 *   104 trames ->  1403 ms
	 *   228 trames ->  3468 ms
	 *
	 * NVIDIA deconseille la rafale quand le rendu tourne sur la meme
	 * machine, pour ne pas affamer le GPU. L'avertissement est prudent et
	 * general ; il ne s'applique pas ici : la 3090 Ti mesure 46 % d'usage
	 * moyen en session, avec 11,6 Go de VRAM libres.
	 *
	 * A rebasculer si l'image saccade pendant que l'agent parle — ce serait
	 * le signe que l'avertissement nous rattrape.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Audio2Face")
	bool bInferenceEnRafale = true;

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

	/**
	 * Delai sans reponse du visiteur avant relance.
	 *
	 * Arme quand l'agent finit de parler, annule des que le visiteur
	 * repond. A echeance, le sidecar recoit `visiteur.silencieux` et
	 * l'agent somme le visiteur de repondre — UNE fois par silence : si le
	 * visiteur se tait encore apres la relance, c'est l'abandon
	 * (DelaiAbandon) qui tranche, sinon les deux minuteries se
	 * repousseraient l'une l'autre indefiniment.
	 */
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

	/**
	 * Marge apres la fin de la lecture pendant laquelle le micro reste sourd.
	 *
	 * La borne ne doit pas s'entendre elle-meme : a volume d'exposition, la
	 * voix TTS captee par le micro est segmentee par Silero comme n'importe
	 * quelle parole, et chaque replique de l'agent repartait au sidecar comme
	 * « reponse » du visiteur — l'interrogatoire pouvait avancer tout seul.
	 * Les segments clos pendant que l'agent est audible sont jetes ; cette
	 * marge court a partir de la fin REELLE de la lecture (suivie par
	 * MinuterieSuiviLecture, ACE jouant son tampon bien apres parole.fin).
	 *
	 * Elle doit rester SUPERIEURE a SilenceFinSegment du micro (0,9 s) : le
	 * VAD ne clot un segment d'echo que ce delai apres le dernier son, et
	 * une marge plus courte — 0,4 s au depart — laissait passer le segment
	 * d'echo lui-meme, le bug exact qu'elle pretendait corriger.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Delais",
		meta = (ClampMin = "0.0", Units = "s"))
	float MargeEchoApresReplique = 1.2f;

	/**
	 * Delai avant de rouvrir une session quand la zone est restee occupee.
	 *
	 * Le capteur ne rend que des FRONTS : apres un abandon, un visiteur
	 * toujours plante devant la borne ne produira jamais de nouveau
	 * presence.detectee, et la borne l'ignorait indefiniment. Au retour en
	 * veille zone occupee, une nouvelle session est reprogrammee ici.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Delais",
		meta = (ClampMin = "1.0", Units = "s"))
	float DelaiReprisePresence = 3.f;

	/**
	 * Delai maximal entre une sollicitation du sidecar et sa reponse.
	 *
	 * La perte de CONNEXION est detectee par la socket, mais un sidecar gele
	 * — processus bloque, socket TCP restee ouverte — passait inapercu :
	 * EstConnecte() restait vrai, la replique de secours (qui exige une
	 * deconnexion) ne partait jamais, et chaque visiteur affrontait
	 * DelaiAbandon secondes de silence, indefiniment.
	 *
	 * Armee a l'envoi de presence.detectee et de chaque enonce ; desarmee
	 * par le parole.debut qui repond. 10 s couvre largement le pire tour
	 * mesure (STT + LLM + TTS ~2,5 s) sans condamner un simple pic de
	 * charge GPU.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Garde Frontiere|Sidecar",
		meta = (ClampMin = "2.0", Units = "s"))
	float DelaiReponseSidecar = 10.f;

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
	UFUNCTION() void SurEmotion(EGuardEmotion Emotion);
	UFUNCTION() void SurPanneIA(const FString& Raison);

	// Minuteries
	void ArmerAbandon();
	void AnnulerMinuteries();
	UFUNCTION() void SurAbandon();
	UFUNCTION() void SurDelaiSortie();
	UFUNCTION() void TenterReconnexion();

	// Surveillance applicative : le sidecar doit repondre, pas seulement
	// rester connecte.
	void ArmerSurveillanceIA();
	void AnnulerSurveillanceIA();
	UFUNCTION() void SurSilenceIA();

	// Relance d'un visiteur muet, au plus une par silence.
	UFUNCTION() void SurSilenceVisiteur();

	// Nouvelle session si la zone est restee occupee au retour en veille.
	UFUNCTION() void SurReprisePresence();

	// Suit la fin REELLE de la lecture apres parole.fin (tampon ACE).
	UFUNCTION() void SurSuiviLecture();

	/**
	 * (Re)demarre le suivi de lecture : estampille l'origine de la marge
	 * anti-echo et arme la sonde.
	 */
	void ArmerSuiviLecture();

	/**
	 * Fait parler la borne SANS le sidecar, et arme le suivi de lecture.
	 *
	 * Les repliques de secours n'ont pas de parole.fin : leur lecture n'a
	 * d'origine de marge anti-echo que si le suivi est arme avec elles.
	 * L'appariement Broadcast + ArmerSuiviLecture etait manuel sur trois
	 * sites, et a derive des sa premiere version — il vit donc ici, et
	 * toute nouvelle replique de secours DOIT passer par cette fonction.
	 */
	void DireRepliqueDeSecours(const FString& Texte);

	/**
	 * Coupe toute lecture en cours : flux, tampon ACE, voix de repli.
	 *
	 * EndAudioSamples signale la fin du flux, il n'arrete PAS la lecture —
	 * ACE continue de jouer son tampon, et Voix->Interrompre() ne coupe
	 * que le repli. Cette connaissance vivait en deux copies (fin de
	 * session, panne IA), et la seconde a d'abord manque le Stop.
	 */
	void CouperLecture();

	/** Glitch puis permutation — ou permutation directe sans effet. */
	void LancerSubstitution();

	/**
	 * L'agent est-il audible en ce moment ?
	 *
	 * bRepliqueEnCours ne couvre que le FLUX (parole.debut -> parole.fin) :
	 * ACE comme Voix continuent de jouer leur tampon apres parole.fin. Pour
	 * savoir si le micro risque de capter la voix de la borne, il faut
	 * interroger les lecteurs eux-memes.
	 */
	bool AgentAudible();

	FTimerHandle MinuterieAbandon;
	FTimerHandle MinuterieSortie;
	FTimerHandle MinuterieReconnexion;
	FTimerHandle MinuterieSurveillanceIA;
	FTimerHandle MinuterieRelance;
	FTimerHandle MinuterieReprise;

	/**
	 * Sonde la lecture apres parole.fin, tant que l'agent reste audible.
	 *
	 * parole.fin clot le FLUX, pas la LECTURE : ACE joue son tampon parfois
	 * plusieurs secondes apres, sans aucun evenement de fin. Sans ce suivi,
	 * la marge anti-echo partait de parole.fin, et un echo clos apres la
	 * vraie fin de lecture passait toujours au travers.
	 */
	FTimerHandle MinuterieSuiviLecture;

	/**
	 * Une relance a deja ete faite pour le silence en cours.
	 *
	 * Remise a faux quand le visiteur parle. Sans ce garde, la relance
	 * repoussait l'abandon (l'agent parle -> ArmerAbandon) qui repoussait la
	 * relance : un visiteur parti sans etre vu du capteur aurait ete
	 * relance en boucle, indefiniment.
	 */
	bool bRelanceFaite = false;

	/**
	 * La panne IA en cours a deja ete annoncee au visiteur.
	 *
	 * SurPanneIA repasse a chaque echec de reconnexion (toutes les
	 * DelaiReconnexion secondes) : sans ce garde, l'agent repetait « Poste
	 * ferme. Repassez plus tard. » en boucle devant le meme visiteur.
	 * Une par PANNE, pas par session : rearme quand le sidecar repond a
	 * nouveau (SurParoleDebut), et au demarrage de session.
	 */
	bool bPanneAnnoncee = false;

	/**
	 * La substitution d'avatar a ete differee : la zone etait encore occupee
	 * a la fin de la session, et glitcher devant temoin montrerait le tour.
	 * Elle sera jouee quand la zone se videra (SurPresencePerdue en veille).
	 */
	bool bSubstitutionDifferee = false;

	/** Dernier instant ou l'agent a ete constate audible (anti-echo). */
	double InstantAgentAudible = -1.0e9;

	/**
	 * Fin ESTIMEE de la lecture Audio2Face de la replique en cours.
	 *
	 * Le composant ACE n'expose pas son etat de lecture (IsPlaybackActive
	 * est prive) : on le deduit de ce qu'on lui envoie nous-memes — ancre
	 * au premier envoi de la replique, majoree de GraceSuiviLecture (le
	 * son part 0,7-1,4 s apres l'envoi, le temps de l'inference), puis
	 * cumul de la duree des echantillons de chaque trame. L'estimation
	 * depasse la realite d'une seconde environ : le micro reste sourd un
	 * peu plus longtemps, jamais moins.
	 */
	double FinLectureAcePresumee = -1.0e9;

	/**
	 * Le suivi de lecture a-t-il deja ENTENDU quelque chose ?
	 *
	 * Tant que non, InstantAgentAudible date de l'armement du suivi : la
	 * meme estampille sert d'origine au delai de grace ci-dessous.
	 */
	bool bLectureObservee = false;

	/**
	 * Temps laisse a la lecture pour DEMARRER apres l'armement du suivi.
	 *
	 * Sur le chemin Audio2Face, le son ne part que 0,7-1,4 s apres
	 * parole.fin (c'est la fermeture de session qui purge les derniers
	 * echantillons, et l'inference suit). Conclure « rien ne joue » a la
	 * premiere sonde muette figeait la marge anti-echo sur parole.fin — et
	 * l'echo de chaque replique courte repassait au travers.
	 */
	static constexpr double GraceSuiviLecture = 2.5;

	/** Dernier « Repetez. » du mode degrade, pour ne pas le marteler. */
	double InstantDernierRepetez = -1.0e9;

	/** Distincte des cles du capteur, pour que les deux messages coexistent. */
	static constexpr uint64 CleAffichagePhase = 8810;
};
