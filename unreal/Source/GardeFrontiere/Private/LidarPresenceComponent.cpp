#include "LidarPresenceComponent.h"

#include "GardeFrontiere.h"
#include "SerialCom.h"
#include "TimerManager.h"
#include "Engine/World.h"

ULidarPresenceComponent::ULidarPresenceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void ULidarPresenceComponent::BeginPlay()
{
	Super::BeginPlay();
	OuvrirPort();
}

void ULidarPresenceComponent::EndPlay(const EEndPlayReason::Type Raison)
{
	if (UWorld* Monde = GetWorld())
	{
		Monde->GetTimerManager().ClearTimer(MinuterieLecture);
		Monde->GetTimerManager().ClearTimer(MinuterieReconnexion);
	}
	FermerPort();
	Super::EndPlay(Raison);
}

// -- Liaison serie -------------------------------------------------------

void ULidarPresenceComponent::OuvrirPort()
{
	bool bOuvert = false;
	Serie = USerialCom::OpenComPort(bOuvert, PortCOM, VitesseBauds);
	bPortOuvert = bOuvert && Serie != nullptr;

	UWorld* Monde = GetWorld();
	if (!Monde)
	{
		return;
	}

	if (bPortOuvert)
	{
		UE_LOG(LogGardeFrontiere, Log,
			TEXT("Capteur : COM%d ouvert a %d bauds"), PortCOM, VitesseBauds);

		Monde->GetTimerManager().SetTimer(
			MinuterieLecture, this, &ULidarPresenceComponent::Lire,
			FMath::Max(PeriodeLecture, 0.02f), true);
	}
	else
	{
		// L'ancien projet abandonnait ici : le capteur debranche condamnait
		// la borne jusqu'au redemarrage. On reessaie indefiniment.
		UE_LOG(LogGardeFrontiere, Warning,
			TEXT("Capteur : COM%d indisponible — nouvelle tentative dans %.0f s"),
			PortCOM, DelaiReconnexion);

		Monde->GetTimerManager().SetTimer(
			MinuterieReconnexion, this, &ULidarPresenceComponent::OuvrirPort,
			FMath::Max(DelaiReconnexion, 1.f), false);
	}
}

void ULidarPresenceComponent::FermerPort()
{
	if (Serie)
	{
		Serie->Close();
		Serie = nullptr;
	}
	bPortOuvert = false;
}

// -- Lecture -------------------------------------------------------------

void ULidarPresenceComponent::Lire()
{
	if (!Serie || !Serie->IsOpened())
	{
		UE_LOG(LogGardeFrontiere, Warning, TEXT("Capteur : liaison perdue"));

		if (UWorld* Monde = GetWorld())
		{
			Monde->GetTimerManager().ClearTimer(MinuterieLecture);
		}
		FermerPort();

		// Un capteur muet ne doit pas laisser un visiteur bloque en session.
		if (bPresent)
		{
			bPresent = false;
			OnPresencePerdue.Broadcast();
		}
		OuvrirPort();
		return;
	}

	bool bSucces = false;
	const FString Ligne = Serie->ReadString(bSucces).TrimStartAndEnd();
	if (!bSucces || Ligne.IsEmpty() || !Ligne.IsNumeric())
	{
		return;   // trame partielle ou bruit : on ignore, sans compter
	}

	const int32 Distance = FCString::Atoi(*Ligne);
	if (Distance <= 0)
	{
		return;
	}
	DerniereDistanceCm = Distance;

	// Hysteresis : le seuil de sortie est plus large que celui d'entree,
	// pour qu'un visiteur pile a la limite ne fasse pas osciller la borne.
	const int32 SeuilSortie = SeuilPresenceCm + HysteresisCm;

	if (Distance <= SeuilPresenceCm)
	{
		CompteurLoin = 0;
		++CompteurProche;

		if (!bPresent && CompteurProche >= ReleveseAvantPresence)
		{
			bPresent = true;
			UE_LOG(LogGardeFrontiere, Log,
				TEXT("Capteur : presence a %d cm"), Distance);
			OnPresenceDetectee.Broadcast();
		}
	}
	else if (Distance > SeuilSortie)
	{
		CompteurProche = 0;
		++CompteurLoin;

		if (bPresent && CompteurLoin >= ReleveseAvantAbsence)
		{
			bPresent = false;
			UE_LOG(LogGardeFrontiere, Log,
				TEXT("Capteur : zone liberee (%d cm)"), Distance);
			OnPresencePerdue.Broadcast();
		}
	}
	// Entre les deux seuils : zone morte, on ne change rien.
}

void ULidarPresenceComponent::ForcerPresence(bool bNouvelEtat)
{
	if (bNouvelEtat == bPresent)
	{
		return;
	}

	bPresent = bNouvelEtat;
	CompteurProche = CompteurLoin = 0;

	UE_LOG(LogGardeFrontiere, Log, TEXT("Capteur : presence forcee a %s"),
		bNouvelEtat ? TEXT("vrai") : TEXT("faux"));

	if (bNouvelEtat) { OnPresenceDetectee.Broadcast(); }
	else             { OnPresencePerdue.Broadcast(); }
}
