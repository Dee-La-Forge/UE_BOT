#pragma once

#include "CoreMinimal.h"

/**
 * Categorie de log dediee.
 *
 * L'ancien projet diagnostiquait par Print String ("BEGINPLAY CAST FAILED",
 * "ERREUR: CLEAN DU CHATBOT") : rien n'etait conserve, et sur une borne en
 * autonomie on ne savait jamais pourquoi elle s'etait figee.
 * Ici tout passe par un log filtrable et archivable.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogGardeFrontiere, Log, All);
