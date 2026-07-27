"""Fabrique des echantillons de voix "visiteur" avec Piper.

Le bench a besoin d'audio reellement transcriptible : un signal synthetique
ne produit aucune transcription, et le pipeline sortirait avant meme
d'atteindre le LLM. On se sert donc du TTS deja installe pour simuler les
reponses du visiteur.

    .venv\\Scripts\\python.exe -m bench.generer_echantillons
"""

from __future__ import annotations

import sys
import wave
from pathlib import Path

import numpy as np
import yaml

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

from src.tts import Synthetiseur  # noqa: E402

# Reponses plausibles d'un visiteur a un controle frontalier.
REPONSES = {
    "motif": "Bonjour, je viens visiter le musee avec ma famille.",
    "duree": "Nous restons deux jours, jusqu'a dimanche soir.",
    "declaration": "Non, je n'ai rien a declarer, juste mon appareil photo.",
    "provenance": "J'arrive de Lyon, en train, ce matin.",
    "hesitation": "Euh... je ne sais pas trop quoi repondre a cette question.",
}


def ecrire_wav(chemin: Path, pcm: np.ndarray, taux: int) -> None:
    """Ecrit du PCM16 mono — le format que Whisper attend."""
    entiers = (np.clip(pcm, -1.0, 1.0) * 32767).astype(np.int16)
    with wave.open(str(chemin), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(taux)
        w.writeframes(entiers.tobytes())


def main() -> int:
    config = yaml.safe_load((RACINE / "config.yaml").read_text(encoding="utf-8"))
    tts = Synthetiseur(config["tts"], RACINE)

    dossier = RACINE / "bench" / "echantillons"
    dossier.mkdir(parents=True, exist_ok=True)

    print(f"  Voix : {config['tts']['voix']}  ({tts.taux} Hz)\n")

    for nom, texte in REPONSES.items():
        parole = tts.synthetiser(texte)
        chemin = dossier / f"{nom}.wav"
        ecrire_wav(chemin, parole.pcm, parole.taux)
        duree = len(parole.pcm) / parole.taux
        print(f"  {chemin.name:<18} {duree:>5.1f} s   {len(parole.visemes):>3} visemes")
        print(f"                     \"{texte}\"")

    print(f"\n  {len(REPONSES)} echantillons dans {dossier.relative_to(RACINE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
