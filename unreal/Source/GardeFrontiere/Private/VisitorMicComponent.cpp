#include "VisitorMicComponent.h"

#include "GardeFrontiere.h"
#include "AudioBridge.h"
#include "Sound/CapturableSoundWave.h"
#include "VAD/RuntimeVADProviderBase.h"

namespace
{
	/** Whisper et Silero travaillent tous deux en 16 kHz mono. */
	constexpr int32 TauxCible = 16000;

	/** Silero decide par trames de 32 ms — 512 echantillons a 16 kHz. */
	constexpr int32 EchantillonsParTrame = 512;
}

UVisitorMicComponent::UVisitorMicComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UVisitorMicComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!bActiverMicro)
	{
		UE_LOG(LogGardeFrontiere, Warning, TEXT("DIAGNOSTIC : micro visiteur desactive"));
		return;
	}

	PreparerVAD();
	OuvrirCapture();
}

void UVisitorMicComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	FermerCapture();
	Super::EndPlay(Raison);
}

// -- Capture -------------------------------------------------------------

void UVisitorMicComponent::PreparerVAD()
{
	// Instanciation par NOM de classe, sans dependre du module Silero.
	// Le detecteur WebRTC integre servirait de repli, mais Silero est
	// nettement plus sur en milieu bruyant — et une borne d'exposition est
	// toujours en milieu bruyant.
	UClass* Classe = FindObject<UClass>(
		nullptr, TEXT("/Script/RuntimeAudioImporterSileroVAD.RuntimeSileroVADProvider"));

	if (!Classe)
	{
		UE_LOG(LogGardeFrontiere, Error,
			TEXT("Micro : fournisseur Silero introuvable — le plugin ")
			TEXT("RuntimeAudioImporterSileroVAD est-il active ? Aucune parole ne sera detectee."));
		return;
	}

	VAD = NewObject<URuntimeVADProviderBase>(this, Classe);
	if (VAD)
	{
		VAD->Reset();
		UE_LOG(LogGardeFrontiere, Log, TEXT("Micro : VAD Silero pret (%d Hz attendus)"),
			VAD->GetRequiredSampleRate());
	}
}

void UVisitorMicComponent::OuvrirCapture()
{
	Micro = UCapturableSoundWave::CreateCapturableSoundWave();
	if (!Micro)
	{
		UE_LOG(LogGardeFrontiere, Error, TEXT("Micro : creation du flux de capture impossible"));
		return;
	}

	// Le delegue natif se declenche sur le thread de capture. On n'y fait
	// donc rien d'autre qu'empiler des flottants sous verrou : toucher a un
	// UObject ou au segment depuis ce thread serait une course.
	TWeakObjectPtr<UVisitorMicComponent> Faible(this);
	PoigneeAudio = Micro->OnPopulateAudioDataNative.AddLambda(
		[Faible](const TArray<float>& PCM)
		{
			UVisitorMicComponent* Comp = Faible.Get();
			if (!Comp || PCM.Num() == 0)
			{
				return;
			}

			FScopeLock Verrouillage(&Comp->Verrou);
			Comp->Entrant.Append(PCM);
		});

	if (!Micro->StartCapture(PeripheriqueMicro))
	{
		UE_LOG(LogGardeFrontiere, Error,
			TEXT("Micro : ouverture du peripherique %d refusee — la borne n'entendra rien"),
			PeripheriqueMicro);
		return;
	}

	bCaptureOuverte = true;
	TauxCapture = Micro->GetSampleRate();
	CanauxCapture = Micro->GetNumOfChannels();

	UE_LOG(LogGardeFrontiere, Log,
		TEXT("Micro : peripherique %d ouvert (%d Hz, %d canal/canaux)"),
		PeripheriqueMicro, TauxCapture, CanauxCapture);
}

void UVisitorMicComponent::FermerCapture()
{
	if (Micro)
	{
		// Detacher AVANT d'arreter : une derniere trame arrivant pendant la
		// fermeture trouverait un composant en cours de destruction.
		Micro->OnPopulateAudioDataNative.Remove(PoigneeAudio);
		PoigneeAudio.Reset();

		Micro->StopCapture();
		Micro->ReleaseMemory();
		Micro = nullptr;
	}

	bCaptureOuverte = false;
	bEcoute = false;
	bParoleEnCours = false;

	FScopeLock Verrouillage(&Verrou);
	Entrant.Reset();
	Segment.Reset();
	Reliquat.Reset();
}

// -- Ecoute --------------------------------------------------------------

void UVisitorMicComponent::DemarrerEcoute()
{
	if (bEcoute)
	{
		return;
	}

	// On jette ce qui a ete capte avant l'arrivee du visiteur : le brouhaha
	// de la salle n'a pas a se retrouver en tete de son premier enonce.
	{
		FScopeLock Verrouillage(&Verrou);
		Entrant.Reset();
	}
	Segment.Reset();
	Reliquat.Reset();
	SilenceEcoule = 0.f;
	bParoleEnCours = false;

	if (VAD)
	{
		VAD->Reset();
	}

	bEcoute = true;
	UE_LOG(LogGardeFrontiere, Log, TEXT("Micro : ecoute ouverte"));
}

void UVisitorMicComponent::ArreterEcoute()
{
	if (!bEcoute)
	{
		return;
	}

	bEcoute = false;
	bParoleEnCours = false;
	Segment.Reset();
	Reliquat.Reset();
	SilenceEcoule = 0.f;

	UE_LOG(LogGardeFrontiere, Log, TEXT("Micro : ecoute fermee"));
}

// -- Traitement ----------------------------------------------------------

void UVisitorMicComponent::FermerSegment()
{
	const float Duree = Segment.Num() / static_cast<float>(TauxCible);

	if (Duree >= DureeMinSegment)
	{
		UE_LOG(LogGardeFrontiere, Log, TEXT("Micro : enonce de %.2f s transmis"), Duree);
		OnSegmentVisiteur.Broadcast(Segment, TauxCible, 1);
	}
	else if (Segment.Num() > 0)
	{
		// Silencieux dans le journal courant : une borne d'exposition capte
		// des dizaines de bruits brefs par heure, et les tracer tous noierait
		// le reste.
		UE_LOG(LogGardeFrontiere, Verbose,
			TEXT("Micro : bruit de %.2f s ignore (minimum %.2f s)"), Duree, DureeMinSegment);
	}

	Segment.Reset();
	SilenceEcoule = 0.f;
	bParoleEnCours = false;
}

void UVisitorMicComponent::TickComponent(
	float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bCaptureOuverte || !Micro)
	{
		return;
	}

	// Purge periodique du tampon interne du plugin. UStreamingSoundWave
	// accumule tout ce qu'il capte, indefiniment : sans cela, une borne qui
	// tourne une journee finit par saturer la memoire.
	const double Maintenant = FPlatformTime::Seconds();
	if (Maintenant - DerniereLiberation >= 10.0)
	{
		DerniereLiberation = Maintenant;
		Micro->ReleaseMemory();
	}

	// Recuperer ce que le thread de capture a depose.
	TArray<float> Brut;
	{
		FScopeLock Verrouillage(&Verrou);
		if (Entrant.Num() == 0)
		{
			return;
		}
		Brut = MoveTemp(Entrant);
		Entrant.Reset();
	}

	// Hors session, on vide sans traiter : le VAD et le reechantillonnage
	// coutent, et personne n'ecoute.
	if (!bEcoute || !VAD)
	{
		return;
	}

	// Une seule conversion, vers le format que veulent le VAD ET Whisper.
	const TArray<float> Mono = UAudioBridge::VersMono(Brut, FMath::Max(CanauxCapture, 1));
	const TArray<float> Reechantillonne =
		UAudioBridge::Reechantillonner(Mono, FMath::Max(TauxCapture, 1), TauxCible);

	Reliquat.Append(Reechantillonne);

	// Le VAD ne decide que sur des trames pleines. Le reliquat attend la
	// prochaine frame plutot que d'etre juge sur un morceau incomplet.
	while (Reliquat.Num() >= EchantillonsParTrame)
	{
		TArray<float> Trame;
		Trame.Append(Reliquat.GetData(), EchantillonsParTrame);
		Reliquat.RemoveAt(0, EchantillonsParTrame, EAllowShrinking::No);

		const int32 Decision = VAD->ProcessAudio(Trame, TauxCible);
		const float DureeTrame = EchantillonsParTrame / static_cast<float>(TauxCible);

		if (Decision < 0)
		{
			continue;   // erreur du fournisseur : on ignore la trame
		}

		if (Decision > 0)
		{
			bParoleEnCours = true;
			SilenceEcoule = 0.f;
			Segment.Append(Trame);
		}
		else if (bParoleEnCours)
		{
			// On garde le silence DANS le segment : Whisper transcrit mieux
			// une phrase qui respire qu'une suite de mots recolles.
			Segment.Append(Trame);
			SilenceEcoule += DureeTrame;

			if (SilenceEcoule >= SilenceFinSegment)
			{
				FermerSegment();
				continue;
			}
		}

		// Garde-fou : un micro qui capte une conversation de salle
		// produirait un enonce sans fin.
		if (bParoleEnCours
			&& Segment.Num() >= static_cast<int32>(DureeMaxSegment * TauxCible))
		{
			UE_LOG(LogGardeFrontiere, Warning,
				TEXT("Micro : enonce coupe a %.0f s — duree maximale atteinte"), DureeMaxSegment);
			FermerSegment();
		}
	}
}
