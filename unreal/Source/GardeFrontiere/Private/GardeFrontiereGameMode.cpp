#include "GardeFrontiereGameMode.h"

AGardeFrontiereGameMode::AGardeFrontiereGameMode()
{
	// Aucun pion impose ici. L'ancien projet chargeait BP_AgentGermain
	// depuis un CONSTRUCTEUR par ConstructorHelpers, et cela chargeait un
	// MetaHuman entier avant que les plugins soient debout — vingt erreurs
	// au demarrage, dont la cascade sur ABP_MH_LiveLink qu'on a mis des
	// jours a attribuer (voir patches/NV_ACE_Reference-UE5.7.md).
	//
	// La lecon vaut pour tout GameMode : ce qui se resout dans un
	// constructeur se resout TROP TOT. Le pion se choisit dans les
	// reglages du projet, ou dans les World Settings de la carte.
	//
	// Rien d'autre a faire : la borne est pilotee par AGuardSessionManager,
	// un acteur de la scene. Le GameMode n'a qu'a ne pas etre celui de
	// Convai.
	PrimaryActorTick.bCanEverTick = false;
}
