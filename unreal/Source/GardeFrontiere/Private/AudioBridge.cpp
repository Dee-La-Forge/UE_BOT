#include "AudioBridge.h"

TArray<float> UAudioBridge::VersMono(const TArray<float>& Entrelace, int32 NbCanaux)
{
	if (NbCanaux <= 1)
	{
		return Entrelace;
	}

	const int32 NbTrames = Entrelace.Num() / NbCanaux;
	TArray<float> Mono;
	Mono.SetNumUninitialized(NbTrames);

	const float Inverse = 1.0f / static_cast<float>(NbCanaux);
	for (int32 i = 0; i < NbTrames; ++i)
	{
		float Somme = 0.f;
		for (int32 c = 0; c < NbCanaux; ++c)
		{
			Somme += Entrelace[i * NbCanaux + c];
		}
		Mono[i] = Somme * Inverse;
	}
	return Mono;
}

TArray<float> UAudioBridge::Reechantillonner(
	const TArray<float>& Entree, int32 TauxSource, int32 TauxCible)
{
	if (TauxSource == TauxCible || Entree.Num() < 2 || TauxSource <= 0 || TauxCible <= 0)
	{
		return Entree;
	}

	const double Rapport = static_cast<double>(TauxCible) / static_cast<double>(TauxSource);
	const int32 NbSortie = FMath::Max(1, FMath::FloorToInt(Entree.Num() * Rapport));

	TArray<float> Sortie;
	Sortie.SetNumUninitialized(NbSortie);

	for (int32 i = 0; i < NbSortie; ++i)
	{
		const double Position = i / Rapport;
		const int32 Index = FMath::FloorToInt(Position);
		const float Fraction = static_cast<float>(Position - Index);

		const float A = Entree[FMath::Min(Index, Entree.Num() - 1)];
		const float B = Entree[FMath::Min(Index + 1, Entree.Num() - 1)];
		Sortie[i] = FMath::Lerp(A, B, Fraction);
	}
	return Sortie;
}

TArray<uint8> UAudioBridge::VersPCM16Visiteur(
	const TArray<float>& Echantillons, int32 TauxSource, int32 NbCanaux)
{
	TArray<uint8> Sortie;
	if (Echantillons.Num() == 0)
	{
		return Sortie;
	}

	const TArray<float> Mono = VersMono(Echantillons, NbCanaux);
	const TArray<float> Cible = Reechantillonner(Mono, TauxSource, GF_TAUX_VISITEUR);

	Sortie.SetNumUninitialized(Cible.Num() * sizeof(int16));
	int16* Ecriture = reinterpret_cast<int16*>(Sortie.GetData());

	for (int32 i = 0; i < Cible.Num(); ++i)
	{
		// Ecretage avant conversion : sans lui, un signal sature reboucle
		// et produit un craquement au lieu d'une simple distorsion.
		const float Valeur = FMath::Clamp(Cible[i], -1.0f, 1.0f);
		Ecriture[i] = static_cast<int16>(Valeur * 32767.0f);
	}
	return Sortie;
}

TArray<float> UAudioBridge::DepuisPCM16(const TArray<uint8>& PCM16)
{
	const int32 NbEchantillons = PCM16.Num() / sizeof(int16);
	TArray<float> Sortie;
	Sortie.SetNumUninitialized(NbEchantillons);

	const int16* Lecture = reinterpret_cast<const int16*>(PCM16.GetData());
	for (int32 i = 0; i < NbEchantillons; ++i)
	{
		Sortie[i] = Lecture[i] / 32768.0f;
	}
	return Sortie;
}

float UAudioBridge::NiveauCrete(const TArray<float>& Echantillons)
{
	float Crete = 0.f;
	for (const float E : Echantillons)
	{
		Crete = FMath::Max(Crete, FMath::Abs(E));
	}
	return FMath::Min(Crete, 1.0f);
}
