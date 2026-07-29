// Capteur de presence sur port serie.
//
// Reprend la logique de BP_LidarManager, qui etait saine, en corrigeant ses
// trois faiblesses :
//
//   1. port COM, vitesse et seuil etaient codes en dur ;
//   2. aucune reconnexion : un debranchement condamnait la borne
//      jusqu'au redemarrage ;
//   3. aucune hysteresis a la detection — seule l'absence etait filtree,
//      ce qui laissait un passant declencher une session.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LidarPresenceComponent.generated.h"

class USerialCom;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnPresenceDetectee);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnPresencePerdue);

UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API ULidarPresenceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULidarPresenceComponent();

	// -- Liaison serie ---------------------------------------------------

	/**
	 * Numero du port serie de l'Arduino.
	 *
	 * A VERIFIER a chaque rebranchement : Windows reattribue le numero, et
	 * le Gestionnaire de peripheriques fait foi (Ports COM et LPT →
	 * "Arduino Uno (COMx)"). Un port absent se reconnait a l'erreur
	 * 00000002, ERROR_FILE_NOT_FOUND.
	 *
	 * 4 le 28/07/2026, releve sur le materiel. La valeur precedente — 5 —
	 * venait du journal de l'ancien projet et n'a jamais correspondu a rien.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Liaison",
		meta = (ClampMin = "1"))
	int32 PortCOM = 4;

	/**
	 * Vitesse de la liaison, en bauds.
	 *
	 * Contrairement au numero de port, elle ne derive pas : c'est le sketch
	 * Arduino qui la fixe. Releve a l'oscilloscope logiciel le 28/07/2026 —
	 * a 115200 les trames sont propres, un entier par ligne termine par \n,
	 * ce qu'attend TraiterReleve. Aux autres vitesses on ne recoit que les
	 * memes octets mal echantillonnes, que le decodage ecarte en silence.
	 *
	 * La valeur precedente — 9600 — venait elle aussi de l'ancien projet.
	 * Le port s'ouvrait, aucune trame n'etait jamais lisible, et rien ne le
	 * signalait.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Liaison")
	int32 VitesseBauds = 115200;

	/** Delai avant nouvelle tentative d'ouverture du port. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Liaison",
		meta = (ClampMin = "1.0", Units = "s"))
	float DelaiReconnexion = 5.f;

	// -- Detection -------------------------------------------------------

	/** Distance en deca de laquelle un visiteur est considere present (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Detection",
		meta = (ClampMin = "1"))
	int32 SeuilPresenceCm = 120;

	/**
	 * Marge ajoutee au seuil pour declarer l'absence.
	 *
	 * Sans elle, un visiteur pile a la limite fait osciller la borne entre
	 * presence et absence a chaque releve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Detection",
		meta = (ClampMin = "0"))
	int32 HysteresisCm = 20;

	/** Releves consecutifs sous le seuil avant de declarer une presence. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Detection",
		meta = (ClampMin = "1"))
	int32 ReleveseAvantPresence = 3;

	/** Releves consecutifs au-dela du seuil avant de declarer une absence. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Detection",
		meta = (ClampMin = "1"))
	int32 ReleveseAvantAbsence = 10;

	/** Periode d'interrogation du capteur. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Detection",
		meta = (ClampMin = "0.02", Units = "s"))
	float PeriodeLecture = 0.1f;

	// -- Etat ------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Capteur|Etat")
	bool bPresent = false;

	UPROPERTY(BlueprintReadOnly, Category = "Capteur|Etat")
	bool bPortOuvert = false;

	UPROPERTY(BlueprintReadOnly, Category = "Capteur|Etat")
	int32 DerniereDistanceCm = -1;

	UPROPERTY(BlueprintAssignable, Category = "Capteur")
	FOnPresenceDetectee OnPresenceDetectee;

	UPROPERTY(BlueprintAssignable, Category = "Capteur")
	FOnPresencePerdue OnPresencePerdue;

	/** Simule une presence sans materiel — mise au point et demonstration. */
	UFUNCTION(BlueprintCallable, Category = "Capteur|Pilotage")
	void ForcerPresence(bool bNouvelEtat);

	/**
	 * Journalise une mesure par seconde, et signale les trames illisibles.
	 *
	 * Sans cette trace, un capteur muet et un capteur mal decode sont
	 * indiscernables : le port s'ouvre, le journal dit "COM4 ouvert", et rien
	 * n'indique qu'aucune trame n'est exploitable. C'est ce qui a rendu une
	 * vitesse de liaison erronee invisible pendant tout un apres-midi.
	 *
	 * Laisse actif pendant la mise au point de la borne. A decocher une fois
	 * les distances calees, sinon le journal grossit d'une ligne par seconde.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Diagnostic")
	bool bTracerReleves = true;

	/**
	 * Mesures rigoureusement identiques avant de conclure a un capteur fige.
	 *
	 * Un telemetre a temps de vol bruite toujours d'un ou deux centimetres,
	 * meme immobile face a un mur. Une valeur constante au centimetre pres,
	 * repetee des centaines de fois, n'est donc pas une mesure : c'est un
	 * module qui a cesse de repondre et dont le sketch reemet sa derniere
	 * valeur connue.
	 *
	 * C'est arrive le 28/07/2026 : 3061 releves a 174 cm sans un ecart, alors
	 * que le capteur avait fonctionne six minutes plus tot. Un reset de
	 * l'Arduino a suffi. Rien ne le signalait — un capteur mort et un couloir
	 * vide produisent la meme trace, et une borne en exposition serait restee
	 * en veille indefiniment sans que personne ne s'en apercoive.
	 *
	 * 300 a 10 Hz, soit trente secondes. Mettre 0 pour desactiver.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Diagnostic",
		meta = (ClampMin = "0"))
	int32 MesuresIdentiquesAvantAlerte = 300;

	/** Vrai quand le capteur est soupconne fige. Repasse a faux des qu'il varie. */
	UPROPERTY(BlueprintReadOnly, Category = "Capteur|Etat")
	bool bCapteurFige = false;

	/**
	 * L'activation gouverne le port : Activate l'ouvre, Deactivate le ferme
	 * — minuteries comprises. Necessaire parce que le SetActive() du
	 * gestionnaire arrive APRES notre BeginPlay : sans ces surcharges, il ne
	 * faisait que basculer un drapeau sans effet sur la liaison.
	 */
	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;

private:
	void OuvrirPort();
	void FermerPort();
	void Lire();
	void TraiterReleve(const FString& Ligne);
	void ProgrammerReconnexion();

	UPROPERTY() TObjectPtr<USerialCom> Serie;

	FTimerHandle MinuterieLecture;
	FTimerHandle MinuterieReconnexion;

	int32 CompteurProche = 0;
	int32 CompteurLoin = 0;

	/** Trames consecutives ecartees, et memoire de la derniere vue. */
	int32 CompteurRejets = 0;
	bool bRejetSignale = false;
	FString DernierRejet;

	/** Horodatage de la derniere mesure tracee, pour limiter a une par seconde. */
	double DerniereTrace = 0.0;

	/**
	 * Mesures retirees du tampon au dernier cycle.
	 *
	 * Doit rester proche de PeriodeLecture x cadence de l'Arduino — environ
	 * neuf a 90 Hz et 10 Hz de lecture. Une valeur qui grimpe signale qu'on
	 * n'arrive plus a vider le tampon aussi vite qu'il se remplit.
	 */
	int32 MesuresParCycle = 0;

	/** Detection du capteur fige : valeur repetee et nombre de repetitions. */
	int32 ValeurRepetee = -1;
	int32 CompteurIdentiques = 0;

	/**
	 * Cles d'affichage a l'ecran.
	 *
	 * Fixes et distinctes : chaque message se remplace au lieu de s'empiler,
	 * et la mesure ne chasse pas l'avertissement de liaison.
	 */
	static constexpr uint64 CleAffichageCapteur = 8801;
	static constexpr uint64 CleAffichageLiaison = 8802;
	static constexpr uint64 CleAffichageFige    = 8803;

	/**
	 * Une lecture est en vol sur un thread de fond.
	 *
	 * USerialCom::ReadStringUntil contient un WaitForSingleObject de
	 * 2 secondes, dans une boucle qui lit octet par octet. Appele depuis le
	 * thread de jeu, il fige boutons et menus. On le deporte donc, et ce
	 * drapeau evite d'empiler les lectures.
	 */
	TAtomic<bool> bLectureEnVol{false};

	/** Espacement croissant des tentatives, pour ne pas saturer le journal. */
	int32 TentativesEchouees = 0;
};
