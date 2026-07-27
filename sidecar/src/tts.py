"""Synthese vocale — Piper sur CPU.

Piper est plus rapide que le temps reel sur CPU et pese ~50 Mo. Il rend la
main phrase par phrase, ce qui est exactement ce dont le pipeline a besoin.

Si la mesure montre que la qualite vocale ne tient pas le personnage,
XTTS-v2 sur GPU est l'alternative : bien meilleure prosodie, clonage de
voix, mais ~2 Go de VRAM et ~300 ms de plus. Les 24 Go disponibles le
permettent — a n'engager qu'apres mesure.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from piper import PiperVoice


class Synthetiseur:
    def __init__(self, config: dict, racine: Path):
        nom = config.get("voix", "fr_FR-siwis-medium")
        modele = racine / "models" / "piper" / f"{nom}.onnx"
        if not modele.exists():
            raise FileNotFoundError(
                f"Voix Piper introuvable : {modele}\n"
                f"Lancer scripts/setup.ps1 pour telecharger les modeles."
            )
        self._voix = PiperVoice.load(str(modele))
        self.taux = self._voix.config.sample_rate

    def synthetiser(self, texte: str) -> np.ndarray:
        """Synthetise une phrase, rendue en PCM float32 mono."""
        morceaux = [
            np.frombuffer(bloc, dtype=np.int16)
            for bloc in self._voix.synthesize_stream_raw(texte)
        ]
        if not morceaux:
            return np.zeros(0, dtype=np.float32)
        return (np.concatenate(morceaux).astype(np.float32) / 32768.0)

    def en_pcm16(self, audio: np.ndarray) -> bytes:
        """Convertit en PCM16 — format attendu par NeuroSync et Unreal."""
        return (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16).tobytes()
