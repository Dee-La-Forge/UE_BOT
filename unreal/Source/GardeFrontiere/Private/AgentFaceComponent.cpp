#include "AgentFaceComponent.h"

#include "GardeFrontiere.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

UAgentFaceComponent::UAgentFaceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UAgentFaceComponent::CiblerMaillage(USkeletalMeshComponent* Maillage)
{
	MaillageFacial = Maillage;
	bAvertissementEmis = false;   // nouveau maillage : on redonne sa chance

	if (Maillage)
	{
		UE_LOG(LogGardeFrontiere, Log, TEXT("Visage : maillage cible = %s"),
			*Maillage->GetName());
		Reinitialiser();
	}
}

bool UAgentFaceComponent::EcrireFlottant(FName Nom, float Valeur)
{
	if (!MaillageFacial.IsValid())
	{
		return false;
	}

	UAnimInstance* Anim = MaillageFacial->GetAnimInstance();
	if (!Anim)
	{
		return false;
	}

	// Reflexion plutot que cast : le C++ ne peut pas referencer
	// Convai_MetaHuman_FaceAnim_C, qui est une classe Blueprint.
	FFloatProperty* Prop = FindFProperty<FFloatProperty>(Anim->GetClass(), Nom);
	if (!Prop)
	{
		return false;
	}

	Prop->SetPropertyValue_InContainer(Anim, Valeur);
	return true;
}

const FMelangeEmotion& UAgentFaceComponent::MelangePour(EGuardEmotion Emotion) const
{
	switch (Emotion)
	{
	case EGuardEmotion::Stare:     return MelangeStare;
	case EGuardEmotion::Concerned: return MelangeConcerned;
	case EGuardEmotion::Angry:     return MelangeAngry;
	case EGuardEmotion::Happy:     return MelangeHappy;
	default:                       return MelangeNeutral;
	}
}

void UAgentFaceComponent::AppliquerEmotion(EGuardEmotion Emotion)
{
	EmotionCourante = Emotion;
	AppliquerMelange(MelangePour(Emotion));

	const UEnum* E = StaticEnum<EGuardEmotion>();
	UE_LOG(LogGardeFrontiere, Verbose, TEXT("Visage : %s"),
		*E->GetDisplayNameTextByValue((int64)Emotion).ToString());
}

void UAgentFaceComponent::AppliquerMelange(const FMelangeEmotion& M)
{
	// "Suprise" est orthographie ainsi dans l'AnimBP — sans le second r.
	// Corriger la faute ici ferait echouer l'ecriture, en silence.
	const TPair<FName, float> Poids[] = {
		{ TEXT("Anger"),   M.Anger   },
		{ TEXT("Joy"),     M.Joy     },
		{ TEXT("Sadness"), M.Sadness },
		{ TEXT("Neutral"), M.Neutral },
		{ TEXT("Afraid"),  M.Afraid  },
		{ TEXT("Suprise"), M.Suprise },
		{ TEXT("Bored"),   M.Bored   },
	};

	int32 Ecrits = 0;
	for (const TPair<FName, float>& P : Poids)
	{
		if (EcrireFlottant(P.Key, P.Value))
		{
			++Ecrits;
		}
	}

	if (VitesseFondu > 0.f)
	{
		EcrireFlottant(TEXT("EmotionLerpAlpha"), VitesseFondu);
	}

	// Une expression qui ne s'applique pas ne provoque aucune erreur : le
	// visage reste simplement fige. On le signale, une fois, plutot que de
	// laisser le defaut passer inapercu.
	if (Ecrits == 0 && !bAvertissementEmis)
	{
		bAvertissementEmis = true;
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Visage : aucune propriete d'emotion trouvee sur l'AnimInstance. ")
			TEXT("Le maillage cible utilise-t-il bien Convai_MetaHuman_FaceAnim ?"));
	}
}

void UAgentFaceComponent::Reinitialiser()
{
	AppliquerEmotion(EGuardEmotion::Neutral);
}
