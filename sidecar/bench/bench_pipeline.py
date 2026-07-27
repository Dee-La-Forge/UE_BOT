"""Bench de latence — l'objet meme du prototype.

Mesure, etage par etage, le delai entre "le visiteur cesse de parler" et
"l'agent emet son premier son". C'est ce chiffre qui doit etre compare aux
2,5 - 4 s estimees de la chaine Convai actuelle.

    py -3.12 -m bench.bench_pipeline --tours 5
    py -3.12 -m bench.bench_pipeline --audio echantillons/reponse1.wav
"""

from __future__ import annotations

import argparse
import asyncio
import statistics
import sys
import wave
from pathlib import Path

import numpy as np
import yaml

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

from src.metrics import Mesure          # noqa: E402
from src.pipeline import MorceauAudio, Pipeline   # noqa: E402


def charger_wav(chemin: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(chemin), "rb") as w:
        taux = w.getframerate()
        brut = w.readframes(w.getnframes())
        audio = np.frombuffer(brut, dtype=np.int16).astype(np.float32) / 32768.0
        if w.getnchannels() == 2:
            audio = audio.reshape(-1, 2).mean(axis=1)
    return audio, taux


def audio_synthetique(secondes: float = 3.0, taux: int = 16000) -> np.ndarray:
    """Signal de repli quand aucun echantillon n'est fourni.

    Whisper n'y trouvera rien d'intelligible : ce mode ne sert qu'a mesurer
    le COUT de l'etage STT, pas sa justesse.
    """
    n = int(secondes * taux)
    t = np.linspace(0, secondes, n, endpoint=False)
    return (0.1 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)


async def executer(tours: int, chemin_audio: Path | None) -> None:
    config = yaml.safe_load((RACINE / "config.yaml").read_text(encoding="utf-8"))

    print("  Initialisation du pipeline...")
    pipeline = Pipeline(config, RACINE)

    if not await pipeline.llm.disponible():
        print(
            f"\n  ERREUR : llama.cpp server injoignable sur {pipeline.llm.url}\n"
            f"  Lancer d'abord :  scripts\\lancer_llm.ps1\n"
        )
        await pipeline.fermer()
        sys.exit(1)

    if chemin_audio:
        audio, taux = charger_wav(chemin_audio)
        print(f"  Echantillon : {chemin_audio.name} ({len(audio) / taux:.1f} s)")
    else:
        audio, taux = audio_synthetique(), 16000
        print("  Audio synthetique (mesure du cout STT, pas de sa justesse)")

    releves: list[float] = []

    for i in range(1, tours + 1):
        print(f"\n  ── Tour {i}/{tours} " + "─" * 30)
        mesure = Mesure()
        phrases: list[str] = []
        finale = None

        async for element in pipeline.tour_de_parole(audio, taux, mesure):
            if isinstance(element, MorceauAudio):
                marque = "  [1er son]" if element.premier else "           "
                print(f"{marque} {element.texte}")
                phrases.append(element.texte)
            else:
                finale = element

        print(mesure.afficher())

        if finale:
            print(f"\n  Emotion : {finale.emotion}    Verdict : {finale.verdict}")

        if (t := mesure.temps_premier_son) is not None:
            releves.append(t)

        if config["mesures"]["actif"]:
            mesure.ecrire(
                RACINE / config["mesures"]["fichier_sortie"],
                contexte={
                    "modele_llm": config["llm"]["modele"],
                    "modele_stt": config["stt"]["modele"],
                    "voix_tts": config["tts"]["voix"],
                    "tour": i,
                },
            )

    if releves:
        print("\n" + "═" * 50)
        print(f"  Temps jusqu'au premier son — sur {len(releves)} tours")
        print("═" * 50)
        print(f"  median   {statistics.median(releves):>8.0f} ms")
        print(f"  min      {min(releves):>8.0f} ms")
        print(f"  max      {max(releves):>8.0f} ms")
        if len(releves) > 1:
            print(f"  ecart-type {statistics.stdev(releves):>6.0f} ms")
        print("\n  Reference Convai actuelle : ~2500 - 4000 ms")

    await pipeline.fermer()


def main() -> None:
    p = argparse.ArgumentParser(description="Bench de latence du sidecar IA locale")
    p.add_argument("--tours", type=int, default=3, help="nombre de mesures")
    p.add_argument("--audio", type=Path, default=None, help="echantillon WAV a transcrire")
    args = p.parse_args()

    if args.audio and not args.audio.exists():
        print(f"  Echantillon introuvable : {args.audio}")
        sys.exit(1)

    asyncio.run(executer(args.tours, args.audio))


if __name__ == "__main__":
    main()
