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

	/** COM5 sur la borne — releve dans le journal de l'ancien projet. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Liaison",
		meta = (ClampMin = "1"))
	int32 PortCOM = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capteur|Liaison")
	int32 VitesseBauds = 9600;

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

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Raison) override;

private:
	void OuvrirPort();
	void FermerPort();
	void Lire();

	UPROPERTY() TObjectPtr<USerialCom> Serie;

	FTimerHandle MinuterieLecture;
	FTimerHandle MinuterieReconnexion;

	int32 CompteurProche = 0;
	int32 CompteurLoin = 0;
};
