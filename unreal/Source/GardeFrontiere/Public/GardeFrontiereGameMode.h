// GameMode du dispositif — successeur de ConvaiDemoGM.
//
// La borne tournait encore sur le GameMode de DEMONSTRATION du plugin
// Convai. Releve au journal le 30/07/2026 :
//
//   LogLoad: Game class is 'ConvaiDemoGM_C'
//   ConvaiFormValidationLog: Warning: Empty API Key, please add it in
//     Edit->Project Settings->Convai
//
// Il instancie un UConvaiPlayerComponent et tente de joindre les serveurs
// Convai au demarrage. Sur une borne dont c'est precisement la dependance
// qu'on a supprimee, garder ce GameMode revient a laisser une porte
// ouverte sur un service resilie — et a afficher ses avertissements.
//
// Celui-ci ne fait rien de particulier, et c'est voulu : toute la logique
// vit dans AGuardSessionManager, pose dans la scene. Un GameMode a sa
// place ici pour une seule raison — ne plus dependre de Convai.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GardeFrontiereGameMode.generated.h"

UCLASS()
class GARDEFRONTIERE_API AGardeFrontiereGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGardeFrontiereGameMode();
};
