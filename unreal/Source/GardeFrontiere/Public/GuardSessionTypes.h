// Types partages de la session de controle.
//
// Etats calques sur le Narrative Design d'origine (docs/NARRATIVE-DESIGN.md) :
//
//     Veille -> Accueil -> Interrogatoire (5 a 10 questions)
//            -> Verdict -> Sortie de zone -> Veille
//
// La phase de sortie n'a jamais transite par le cloud : elle etait deja
// geree cote Unreal, et le reste.

#pragma once

#include "CoreMinimal.h"
#include "GuardSessionTypes.generated.h"

/** Etat courant de la borne. */
UENUM(BlueprintType)
enum class EGuardPhase : uint8
{
	/** Personne devant le capteur. L'agent attend. */
	Veille          UMETA(DisplayName = "Veille"),

	/** Visiteur detecte : glitch, changement d'avatar, prise de parole. */
	Accueil         UMETA(DisplayName = "Accueil"),

	/** Questions successives, une a la fois. */
	Interrogatoire  UMETA(DisplayName = "Interrogatoire"),

	/** Verdict rendu, tampon affiche. */
	Verdict         UMETA(DisplayName = "Verdict"),

	/** L'agent invite le visiteur a liberer la zone. */
	SortieZone      UMETA(DisplayName = "Sortie de zone"),
};

/** Decision finale du controle. */
UENUM(BlueprintType)
enum class EGuardVerdict : uint8
{
	EnCours  UMETA(DisplayName = "En cours"),
	Accepte  UMETA(DisplayName = "Accepte"),
	Refuse   UMETA(DisplayName = "Refuse"),
};

/**
 * Emotion de l'agent.
 *
 * Aligne sur l'enum E_Emotions du plugin Convai et sur les 16 poses de
 * Motions2/Face/ (Angry, Concerned, Happy, Neutral, Stare x 3 intensites).
 * Le sidecar l'emet dans le tag [EMOTION:...] de chaque replique.
 */
UENUM(BlueprintType)
enum class EGuardEmotion : uint8
{
	Neutral    UMETA(DisplayName = "Neutre"),
	Stare      UMETA(DisplayName = "Jauge"),
	Concerned  UMETA(DisplayName = "Soupconneux"),
	Angry      UMETA(DisplayName = "En colere"),
	Happy      UMETA(DisplayName = "Satisfait"),
};

/** Pourquoi une session s'est terminee — utile au diagnostic sur site. */
UENUM(BlueprintType)
enum class EGuardFinDeSession : uint8
{
	/** Deroulement normal : verdict rendu, visiteur parti. */
	Nominale       UMETA(DisplayName = "Nominale"),

	/** Le visiteur a quitte la zone avant la fin. */
	Abandon        UMETA(DisplayName = "Abandon"),

	/** Aucune reponse dans le delai imparti. */
	Timeout        UMETA(DisplayName = "Timeout"),

	/** Le sidecar est injoignable ou a echoue. */
	PanneIA        UMETA(DisplayName = "Panne IA"),
};
