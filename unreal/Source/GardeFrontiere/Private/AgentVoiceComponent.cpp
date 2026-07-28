#include "AgentVoiceComponent.h"

#include "GardeFrontiere.h"
#include "Sound/SoundWaveProcedural.h"

UAgentVoiceComponent::UAgentVoiceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// La voix ne doit pas demarrer seule : elle attend les trames du sidecar.
	bAutoActivate = false;
	bAlwaysPlay = true;
}

void UAgentVoiceComponent::BeginPlay()
{
	Super::BeginPlay();
	SetComponentTickEnabled(true);
}

void UAgentVoiceComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	Interrompre();
	Super::EndPlay(Raison);
}

void UAgentVoiceComponent::PreparerFlux(int32 Taux)
{
	// Le taux peut changer d'une session a l'autre si on change de voix.
	// On reconstruit alors le flux plutot que de lire a la mauvaise vitesse.
	if (Flux && TauxCourant == Taux)
	{
		return;
	}

	Stop();

	Flux = NewObject<USoundWaveProcedural>(this);
	Flux->SetSampleRate(Taux);
	Flux->NumChannels = 1;
	Flux->Duration = INDEFINITELY_LOOPING_DURATION;   // flux, pas fichier
	Flux->SoundGroup = SOUNDGROUP_Voice;
	Flux->bLooping = false;
	Flux->bProcedural = true;

	TauxCourant = Taux;
	SetSound(Flux);

	UE_LOG(LogGardeFrontiere, Verbose, TEXT("Voix : flux prepare a %d Hz"), Taux);
}

void UAgentVoiceComponent::EmpilerTrame(const TArray<uint8>& PCM16, int32 Taux)
{
	if (PCM16.Num() == 0 || Taux <= 0)
	{
		return;
	}

	PreparerFlux(Taux);
	if (!Flux)
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Voix : flux indisponible"));
		return;
	}

	Flux->QueueAudio(PCM16.GetData(), PCM16.Num());
	SilenceEcoule = 0.f;

	if (!bParle)
	{
		bParle = true;
		Play();
		UE_LOG(LogGardeFrontiere, Log, TEXT("Voix : l'agent prend la parole"));
		OnVoixDemarree.Broadcast();
	}
}

void UAgentVoiceComponent::Interrompre()
{
	// Arreter AVANT de vider la file. ResetAudio prend un verrou que le
	// thread de rendu audio detient aussi : le solliciter pendant que la
	// lecture tourne est un interblocage classique.
	Stop();

	if (Flux)
	{
		Flux->ResetAudio();
	}

	if (bParle)
	{
		bParle = false;
		SilenceEcoule = 0.f;
		OnVoixTerminee.Broadcast();
	}
}

int32 UAgentVoiceComponent::OctetsEnAttente() const
{
	return Flux ? Flux->GetAvailableAudioByteCount() : 0;
}

void UAgentVoiceComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bParle)
	{
		return;
	}

	// La file peut se vider brievement entre deux phrases du flux : le TTS
	// synthetise la suivante pendant qu'on lit la precedente. On n'annonce
	// donc la fin de parole qu'apres un silence franc.
	if (OctetsEnAttente() > 0)
	{
		SilenceEcoule = 0.f;
		return;
	}

	SilenceEcoule += DeltaTime;
	if (SilenceEcoule >= DelaiFinDeParole)
	{
		bParle = false;
		SilenceEcoule = 0.f;
		UE_LOG(LogGardeFrontiere, Log, TEXT("Voix : fin de replique"));
		OnVoixTerminee.Broadcast();
	}
}
