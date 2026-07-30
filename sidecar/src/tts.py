"""Synthese vocale — Piper 1.6 sur CPU.

Piper est plus rapide que le temps reel sur CPU et pese ~60 Mo. Il rend la
main phrase par phrase, ce qui est exactement ce dont le pipeline a besoin.

Le lipsync ne passe PAS par ici : il est assure par Audio2Face, cote
Unreal, a partir des trames audio (docs/LIPSYNC-DECISION.md). L'ancien
chemin de repli — alignements de phonemes Piper vers les poses MHF_* du
plugin Convai — est SUPPRIME : les poses n'existaient pas sur l'AnimBP, et
la machinerie restee en place (patch ONNX au chargement, frise de visemes
calculee puis jetee) ne faisait que couter et tromper.

Si la qualite vocale ne tient pas le personnage, XTTS-v2 sur GPU reste
l'alternative (~2 Go de VRAM, ~300 ms de plus). Les 24 Go disponibles le
permettent — a n'engager qu'apres mesure.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np
from piper import PiperVoice, SynthesisConfig


@dataclass
class Parole:
    """Une phrase synthetisee."""

    pcm: np.ndarray   # float32 mono, dans [-1, 1]
    taux: int


class Synthetiseur:
    def __init__(self, config: dict, racine: Path):
        # Fonctionnalite retiree : refuser franchement vaut mieux que de
        # replonger en silence dans un chemin mort (le patch ONNX exigeait
        # en prime le paquet `onnx`, absent de requirements.txt — le
        # sidecar plantait au demarrage).
        if config.get("phonemes_pour_repli"):
            raise RuntimeError(
                "tts.phonemes_pour_repli : fonctionnalite retiree — le "
                "lipsync vient d'Audio2Face, cote Unreal "
                "(docs/LIPSYNC-DECISION.md). Supprimer la cle."
            )

        nom = config.get("voix", "fr_FR-siwis-medium")
        modele = racine / "models" / "piper" / f"{nom}.onnx"
        if not modele.exists():
            raise FileNotFoundError(
                f"Voix Piper introuvable : {modele}\n"
                f"Lancer scripts/telecharger.sh pour recuperer les modeles."
            )

        self._voix = PiperVoice.load(str(modele))
        self.taux = self._voix.config.sample_rate

        vitesse = config.get("vitesse", 1.0)
        self._config = SynthesisConfig(
            # length_scale > 1 ralentit ; on inverse pour raisonner en vitesse.
            length_scale=(1.0 / vitesse) if vitesse and vitesse != 1.0 else None,
            volume=config.get("volume", 1.0),
        )

    def synthetiser(self, texte: str) -> Parole:
        """Synthetise une phrase et rend l'audio."""
        morceaux = list(self._voix.synthesize(texte, syn_config=self._config))
        if not morceaux:
            return Parole(pcm=np.zeros(0, dtype=np.float32), taux=self.taux)

        pcm = np.concatenate([m.audio_float_array for m in morceaux]).astype(np.float32)
        return Parole(pcm=pcm, taux=morceaux[0].sample_rate)

    @staticmethod
    def en_pcm16(audio: np.ndarray) -> bytes:
        """Convertit en PCM16 — le format des trames du contrat."""
        return (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16).tobytes()
