#include "AgentFaceComponent.h"

#include "GardeFrontiere.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

UAgentFaceComponent::UAgentFaceComponent()
{
	// Le lipsync a besoin d'une horloge : les poses durent 60 a 120 ms et se
	// recouvrent. Le tick reste inerte tant qu'aucune frise n'est planifiee.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

// -- Lipsync -------------------------------------------------------------

void UAgentFaceComponent::PlanifierVisemes(
	const TArray<FGuardViseme>& Visemes, float DelaiAvantLecture)
{
	if (Visemes.Num() == 0)
	{
		return;
	}

	// Rien ne joue : l'horloge repart de zero avec cette trame. Sinon on
	// empile a la suite, sur l'horloge deja en cours.
	if (VisemesPlanifies.Num() == 0 && PosesActives.Num() == 0)
	{
		TempsLipsync = 0.f;
	}

	const float Origine = TempsLipsync + FMath::Max(0.f, DelaiAvantLecture);

	VisemesPlanifies.Reserve(VisemesPlanifies.Num() + Visemes.Num());
	for (const FGuardViseme& V : Visemes)
	{
		FGuardViseme Decale = V;
		Decale.Debut = Origine + V.Debut;
		Decale.Fin = Origine + V.Fin;
		VisemesPlanifies.Add(Decale);
	}

	SetComponentTickEnabled(true);
}

void UAgentFaceComponent::ArreterVisemes()
{
	VisemesPlanifies.Reset();

	// Refermer ce qui restait ouvert : une bouche figee sur un "Ah" au
	// moment ou l'agent se tait est pire que pas de lipsync du tout.
	for (const FName& Pose : PosesActives)
	{
		EcrireFlottant(Pose, 0.f);
	}
	PosesActives.Reset();

	TempsLipsync = 0.f;
	SetComponentTickEnabled(false);
}

float UAgentFaceComponent::PoidsViseme(const FGuardViseme& V, float Temps) const
{
	if (Temps <= V.Debut || Temps >= V.Fin)
	{
		return 0.f;
	}

	// Le fondu ne peut pas depasser la moitie de la pose, sinon l'ouverture
	// et la fermeture se chevaucheraient et la pose n'atteindrait jamais son
	// amplitude.
	const float Duree = V.Fin - V.Debut;
	const float Fondu = FMath::Min(FonduViseme, Duree * 0.5f);

	if (Fondu <= KINDA_SMALL_NUMBER)
	{
		return AmplitudeViseme;
	}

	const float DepuisDebut = Temps - V.Debut;
	const float AvantFin = V.Fin - Temps;
	const float Facteur = FMath::Min(
		FMath::Min(DepuisDebut, AvantFin) / Fondu, 1.f);

	return AmplitudeViseme * Facteur;
}

void UAgentFaceComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	TempsLipsync += DeltaTime;

	// Poids cumules : deux poses voisines se recouvrent, et c'est ce
	// recouvrement qui fait la fluidite de l'articulation.
	TMap<FName, float> Poids;
	int32 Restants = 0;

	for (const FGuardViseme& V : VisemesPlanifies)
	{
		if (TempsLipsync < V.Fin)
		{
			++Restants;
		}

		const float P = PoidsViseme(V, TempsLipsync);
		if (P > 0.f)
		{
			float& Cumul = Poids.FindOrAdd(V.Pose, 0.f);
			Cumul = FMath::Max(Cumul, P);
		}
	}

	for (const TPair<FName, float>& Paire : Poids)
	{
		EcrireFlottant(Paire.Key, Paire.Value);
		PosesActives.Add(Paire.Key);
	}

	// Refermer celles qui viennent de s'eteindre — sans quoi elles
	// resteraient ouvertes a leur derniere valeur.
	for (auto It = PosesActives.CreateIterator(); It; ++It)
	{
		if (!Poids.Contains(*It))
		{
			EcrireFlottant(*It, 0.f);
			It.RemoveCurrent();
		}
	}

	// La frise est epuisee : on purge et on s'arrete plutot que de parcourir
	// indefiniment un tableau de poses passees.
	if (Restants == 0)
	{
		VisemesPlanifies.Reset();
		if (PosesActives.Num() == 0)
		{
			TempsLipsync = 0.f;
			SetComponentTickEnabled(false);
		}
	}
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
		// Un nom introuvable est le seul mode de panne silencieux de tout le
		// lipsync : la fonction rend false, personne ne le lit, et la bouche
		// reste fermee sans qu'aucune ligne ne l'explique. On le dit — une
		// fois par nom, sinon ce serait dix lignes par seconde.
		if (!NomsIntrouvables.Contains(Nom))
		{
			NomsIntrouvables.Add(Nom);
			UE_LOG(LogGardeFrontiere, Warning,
				TEXT("Visage : '%s' absent de %s — la pose ne sera jamais ecrite. ")
				TEXT("Verifier les variables flottantes de l'AnimBP facial."),
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
	// La bouche aussi : un visage remis au neutre en gardant un viseme ouvert
	// laisserait l'agent figé sur une voyelle apres le depart du visiteur.
	ArreterVisemes();
	AppliquerEmotion(EGuardEmotion::Neutral);
}
