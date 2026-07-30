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

	// Le format n'est PAS connu ici. Le plugin construit son flux avec un
	// taux nul et ne le renseigne qu'a l'arrivee du premier buffer capture :
	// lire GetSampleRate() juste apres StartCapture rend 0. Figer ce 0
	// faisait reechantillonner chaque drain d'un facteur 16000 — gel du
	// thread de jeu puis saturation memoire des la premiere parole. Le
	// format se releve au premier drain, dans TickComponent.
	TauxCapture = 0;
	CanauxCapture = 0;

	UE_LOG(LogGardeFrontiere, Log,
		TEXT("Micro : peripherique %d ouvert — format connu au premier buffer"),
		PeripheriqueMicro);
}

void UVisitorMicComponent::FermerCapture()
{
	if (Micro)
	{
		// Couper la SOURCE d'abord, detacher ensuite. TMulticastDelegate
		// n'est pas thread-safe : un Remove execute sur le thread de jeu
		// pendant qu'un Broadcast du thread de capture parcourt la liste
		// d'invocation peut la corrompre — crash rare et irreproductible
		// sur une borne a milliers de sessions. StopCapture tarit le flux ;
		// l'eventuel dernier callback en vol est tolere par le
		// TWeakObjectPtr du lambda.
		Micro->StopCapture();

		Micro->OnPopulateAudioDataNative.Remove(PoigneeAudio);
		PoigneeAudio.Reset();

		Micro->ReleaseMemory();
		Micro = nullptr;
	}

	bCaptureOuverte = false;
	bEcoute = false;
	bParoleEnCours = false;
	TauxCapture = 0;
	CanauxCapture = 0;

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
	const float DureeBrute = Segment.Num() / static_cast<float>(TauxCible);

	// Elaguer le silence de queue AVANT de transmettre.
	//
	// SilenceEcoule est exactement ce qu'on a accumule depuis la derniere
	// trame de parole : on en garde une marge courte, on jette le reste. La
	// decision de fin d'enonce reste prise sur SilenceFinSegment, genereux ;
	// seule la quantite transmise change.
	//
	// Sans cela, chaque enonce portait 600 ms de vide que Whisper transcrivait
	// — deux tiers du signal sur les repliques courtes.
	const float ASupprimer = SilenceEcoule - MargeSilenceTransmise;
	if (ASupprimer > 0.f)
	{
		const int32 Echantillons = FMath::Min(
			static_cast<int32>(ASupprimer * TauxCible), Segment.Num());
		Segment.SetNum(Segment.Num() - Echantillons, EAllowShrinking::No);
	}

	const float Duree = Segment.Num() / static_cast<float>(TauxCible);

	if (Duree >= DureeMinSegment)
	{
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Micro : enonce de %.2f s transmis (%.2f s avant elagage)"),
			Duree, DureeBrute);
		OnSegmentVisiteur.Broadcast(Segment, TauxCible, 1);
	}
	else if (Segment.Num() > 0)
	{
		// En Log et non en Verbose. Le rejet etait invisible, et j'ai cru
		// pendant un temps qu'aucun fragment n'etait ecarte — alors que le
		// seuil etait simplement trop bas pour en ecarter un seul. Un rejet
		// qui ne se voit pas ne se regle pas.
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Micro : fragment de %.2f s ignore (minimum %.2f s) — trop court pour Whisper"),
			Duree, DureeMinSegment);
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

	// Premier drain : un buffer au moins a ete capte, le plugin connait donc
	// enfin son format. C'est ICI qu'on le releve — a l'ouverture il valait
	// encore 0, et reechantillonner sur un format invente diluait le signal.
	if (TauxCapture <= 0 || CanauxCapture <= 0)
	{
		TauxCapture = Micro->GetSampleRate();
		CanauxCapture = Micro->GetNumOfChannels();

		if (TauxCapture <= 0 || CanauxCapture <= 0)
		{
			// Toujours inconnu : on jette ce drain plutot que de le traiter
			// sur un format faux.
			return;
		}

		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Micro : format de capture releve — %d Hz, %d canal/canaux"),
			TauxCapture, CanauxCapture);
	}

	// Une seule conversion, vers le format que veulent le VAD ET Whisper.
	const TArray<float> Mono = UAudioBridge::VersMono(Brut, CanauxCapture);
	const TArray<float> Reechantillonne =
		UAudioBridge::Reechantillonner(Mono, TauxCapture, TauxCible);

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
