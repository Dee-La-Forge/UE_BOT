#include "GlitchComponent.h"

#include "GardeFrontiere.h"
#include "Engine/PostProcessVolume.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Curves/CurveFloat.h"
#include "TimerManager.h"
#include "Engine/World.h"

UGlitchComponent::UGlitchComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UGlitchComponent::BeginPlay()
{
	Super::BeginPlay();
	PreparerMateriau();
}

void UGlitchComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	Arreter();
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearTimer(MinuterieRetard);
	}
	Super::EndPlay(Raison);
}

bool UGlitchComponent::PreparerMateriau()
{
	if (!MateriauGlitch)
	{
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Glitch : aucun materiau assigne (attendu : PPM_GLITCH)"));
		return false;
	}

	VolumeResolu = VolumeCible;

	if (!VolumeResolu)
	{
		// Repli sur l'ancien comportement : le premier volume trouve.
		// Signale, parce que c'est precisement ce qu'on cherche a eviter.
		TArray<AActor*> Volumes;
		UGameplayStatics::GetAllActorsOfClass(this, APostProcessVolume::StaticClass(), Volumes);
		if (Volumes.Num() > 0)
		{
			VolumeResolu = Cast<APostProcessVolume>(Volumes[0]);
			UE_LOG(LogGardeFrontiere, Warning,
				TEXT("Glitch : aucun volume designe, repli sur le premier trouve (%s) ")
				TEXT("parmi %d. Assigner VolumeCible pour un resultat stable."),
				*VolumeResolu->GetName(), Volumes.Num());
		}
	}

	if (!VolumeResolu)
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Glitch : aucun PostProcessVolume dans le niveau"));
		return false;
	}

	MateriauDynamique = UMaterialInstanceDynamic::Create(MateriauGlitch, this);
	if (!MateriauDynamique)
	{
		return false;
	}

	AppliquerIntensite(0.f);
	BrancherSurVolume(0.f);   // present mais inactif

	UE_LOG(LogGardeFrontiere, Log, TEXT("Glitch : pret sur %s"), *VolumeResolu->GetName());
	return true;
}

void UGlitchComponent::BrancherSurVolume(float Poids)
{
	if (VolumeResolu && MateriauDynamique)
	{
		VolumeResolu->AddOrUpdateBlendable(MateriauDynamique, Poids);
	}
}

void UGlitchComponent::AppliquerIntensite(float Valeur)
{
	if (MateriauDynamique)
	{
		MateriauDynamique->SetScalarParameterValue(ParametreIntensite, Valeur);
	}
}

bool UGlitchComponent::EstEnCours() const
{
	if (bEnCours)
	{
		return true;
	}

	// Le retard d'amorcage compte : la substitution est engagee des
	// Declencher(), meme si rien n'est encore visible.
	const UWorld* Monde = GetWorld();
	return Monde && Monde->GetTimerManager().IsTimerActive(MinuterieRetard);
}

void UGlitchComponent::Declencher()
{
	if (bEnCours)
	{
		return;
	}
	if (!MateriauDynamique && !PreparerMateriau())
	{
		// Sans effet visuel, on ne bloque pas la suite : l'avatar doit
		// changer meme si le glitch est indisponible.
		OnGlitchTermine.Broadcast();
		return;
	}

	if (RetardAvantEffet > 0.f)
	{
		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().SetTimer(
				MinuterieRetard, this, &UGlitchComponent::Demarrer, RetardAvantEffet, false);
			return;
		}
	}
	Demarrer();
}

void UGlitchComponent::Demarrer()
{
	bEnCours = true;
	Ecoule = 0.f;

	BrancherSurVolume(1.f);   // le materiau agit desormais sur le volume

	if (SonGlitch)
	{
		UGameplayStatics::PlaySound2D(this, SonGlitch);
	}

	SetComponentTickEnabled(true);
	UE_LOG(LogGardeFrontiere, Log, TEXT("Glitch : demarre (%.2f s)"), Duree);
}

void UGlitchComponent::Arreter()
{
	// AVANT le garde-fou sur bEnCours : pendant RetardAvantEffet, l'effet
	// n'a pas encore commence — bEnCours est faux — mais la minuterie court.
	// Sortir ici la laissait vivre, et le glitch se declenchait tout seul
	// apres la fin de la session, devant une borne retournee en veille.
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearTimer(MinuterieRetard);
	}

	if (!bEnCours)
	{
		return;
	}

	bEnCours = false;
	SetComponentTickEnabled(false);

	// Les deux leviers, dans cet ordre : on eteint l'intensite avant de
	// debrancher, sinon une frame peut afficher le materiau a plein.
	AppliquerIntensite(0.f);
	BrancherSurVolume(0.f);
}

void UGlitchComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnCours)
	{
		return;
	}

	Ecoule += DeltaTime;
	const float Avancement = FMath::Clamp(Ecoule / FMath::Max(Duree, 0.05f), 0.f, 1.f);

	// Sans courbe : montee puis descente symetriques.
	const float Intensite = CourbeIntensite
		? CourbeIntensite->GetFloatValue(Avancement)
		: 1.f - FMath::Abs(Avancement * 2.f - 1.f);

	AppliquerIntensite(Intensite);

	if (Avancement >= 1.f)
	{
		Arreter();
		UE_LOG(LogGardeFrontiere, Log, TEXT("Glitch : termine"));
		OnGlitchTermine.Broadcast();
	}
}
