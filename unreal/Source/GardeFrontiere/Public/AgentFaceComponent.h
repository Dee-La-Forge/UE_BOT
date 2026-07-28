// Expression faciale de l'agent.
//
// L'AnimBP Convai_MetaHuman_FaceAnim n'expose PAS un enum d'etat mais des
// **poids flottants independants**, un par emotion :
//
//   Anger · Joy · Sadness · Neutral · Afraid · Suprise · Bored
//   EmotionLerpAlpha (vitesse de fondu)
//
// C'est bien plus riche qu'un selecteur : 0,6 de neutre + 0,3 de colere
// donne un agent contrarie mais contenu, ce qu'un enum ne permet pas.
//
// Les proprietes sont ecrites par reflexion plutot qu'en castant vers la
// classe Blueprint : le C++ ne peut pas referencer Convai_MetaHuman_FaceAnim_C,
// et cette approche survit a une recompilation de l'AnimBP.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GuardSessionTypes.h"
#include "AgentFaceComponent.generated.h"

class USkeletalMeshComponent;

/** Melange de poids a appliquer pour une emotion donnee. */
USTRUCT(BlueprintType)
struct FMelangeEmotion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Anger = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Joy = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Sadness = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Neutral = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Afraid = 0.f;

	/** Orthographe volontaire : l'AnimBP ecrit "Suprise", sans le second r. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Suprise = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Bored = 0.f;
};

UCLASS(ClassGroup = (GardeFrontiere), meta = (BlueprintSpawnableComponent))
class GARDEFRONTIERE_API UAgentFaceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAgentFaceComponent();

	/** Maillage facial du MetaHuman actif. A renseigner apres chaque switch. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Visage")
	void CiblerMaillage(USkeletalMeshComponent* Maillage);

	/** Applique l'emotion correspondant au tag emis par le LLM. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Visage")
	void AppliquerEmotion(EGuardEmotion Emotion);

	/** Applique un melange arbitraire — reglages fins et mise au point. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Visage")
	void AppliquerMelange(const FMelangeEmotion& Melange);

	/** Remet le visage au neutre. */
	UFUNCTION(BlueprintCallable, Category = "Garde Frontiere|Visage")
	void Reinitialiser();

	// -- Correspondance tag -> melange, editable sans recompiler ----------
	//
	// Les valeurs par defaut traduisent le personnage : un garde-frontiere
	// froid, qui ne sourit jamais franchement.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Visage|Melanges")
	FMelangeEmotion MelangeNeutral{0.f, 0.f, 0.f, 1.0f, 0.f, 0.f, 0.f};

	/** Il jauge, detache. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Visage|Melanges")
	FMelangeEmotion MelangeStare{0.f, 0.f, 0.f, 0.7f, 0.f, 0.f, 0.3f};

	/** Contrarie sans exploser. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Visage|Melanges")
	FMelangeEmotion MelangeConcerned{0.3f, 0.f, 0.3f, 0.4f, 0.f, 0.f, 0.f};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Visage|Melanges")
	FMelangeEmotion MelangeAngry{1.0f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};

	/** Jamais un franc sourire : le personnage ne felicite pas. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Visage|Melanges")
	FMelangeEmotion MelangeHappy{0.f, 0.7f, 0.f, 0.3f, 0.f, 0.f, 0.f};

	/** Vitesse de fondu entre deux emotions. 0 = laisser la valeur de l'AnimBP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Garde Frontiere|Visage",
		meta = (ClampMin = "0.0"))
	float VitesseFondu = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Garde Frontiere|Visage")
	EGuardEmotion EmotionCourante = EGuardEmotion::Neutral;

private:
	/** Ecrit un flottant sur l'AnimInstance par reflexion. */
	bool EcrireFlottant(FName Nom, float Valeur);

	const FMelangeEmotion& MelangePour(EGuardEmotion Emotion) const;

	UPROPERTY() TWeakObjectPtr<USkeletalMeshComponent> MaillageFacial;

	/** Averti une seule fois si l'AnimBP n'expose pas les proprietes. */
	bool bAvertissementEmis = false;
};
