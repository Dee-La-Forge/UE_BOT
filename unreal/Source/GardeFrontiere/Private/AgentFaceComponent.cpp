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
		// Un nom introuvable ne provoque aucune erreur : la fonction rend
		// false et personne ne le lit. C'est ce silence qui a masque un
		// chemin de lipsync entier ecrivant dans le vide. On le dit — une
		// fois par nom, sinon ce serait dix lignes par seconde.
		//
		// En VERBOSE et non en Warning : avec Face_AnimBP, les sept
		// variables de Convai sont absentes, et c'est ATTENDU — l'emotion
		// passe par Audio2Face (docs/EMOTIONS-VS-LIPSYNC.md). Sept
		// avertissements par session pour un fonctionnement nominal, c'est
		// le genre de bruit qui fait qu'on cesse de lire les journaux. Le
		// resume d'AppliquerMelange, lui, reste en Warning : il ne se
		// declenche que si RIEN ne s'ecrit, ce qui est un vrai defaut.
		if (!NomsIntrouvables.Contains(Nom))
		{
			NomsIntrouvables.Add(Nom);
			UE_LOG(LogGardeFrontiere, Verbose,
				TEXT("Visage : '%s' absent de %s — la valeur ne sera jamais ecrite."),
				*Nom.ToString(), *Anim->GetClass()->GetName());
		}
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
	// ATTENTION : ces sept variables sont celles de
	// Convai_MetaHuman_FaceAnim. Face_AnimBP — l'AnimBP de NVIDIA que
	// PreparerAudio2Face impose au spawn — n'en connait AUCUNE : depuis le
	// montage Audio2Face, ces ecritures partent donc toutes dans le vide,
	// et le visage garde une expression fixe. Voir
	// docs/EMOTIONS-VS-LIPSYNC.md : la decision (ajouter les variables a
	// Face_AnimBP, ou laisser ACE piloter l'expression) n'est pas prise.
	//
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
	// NB : EcrireFlottant journalise chaque variable absente en Verbose et
	// non en Warning. Avec Face_AnimBP, les SEPT le sont — c'est attendu
	// (docs/EMOTIONS-VS-LIPSYNC.md), et sept avertissements par replique
	// ne feraient que noyer le journal. Le resume ci-dessous suffit.

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
			TEXT("Attendu avec Face_AnimBP (NVIDIA), qui n'expose pas les ")
			TEXT("variables de Convai — voir docs/EMOTIONS-VS-LIPSYNC.md."));
	}
}

void UAgentFaceComponent::Reinitialiser()
{
	AppliquerEmotion(EGuardEmotion::Neutral);
}
