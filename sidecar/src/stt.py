"""Transcription — faster-whisper sur CPU.

Sur CPU par choix, pas par contrainte : les 64 Go de RAM sont sous-employes
et cela laisse le GPU entierement au rendu Unreal et a NeuroSync.

L'endpointing (savoir quand le visiteur a fini de parler) est deja resolu en
amont par SileroVAD, cote Unreal. On ne recoit donc que des segments utiles,
ce qui evite de transcrire du silence.
"""

from __future__ import annotations

import numpy as np
from faster_whisper import WhisperModel


class Transcripteur:
    def __init__(self, config: dict):
        self.langue = config.get("langue", "fr")
        self._vad = config.get("vad_filtre", True)
        self._modele = WhisperModel(
            config.get("modele", "small"),
            device=config.get("peripherique", "cpu"),
            compute_type=config.get("type_calcul", "int8"),
        )

    def transcrire(self, audio: np.ndarray, taux: int = 16000) -> str:
        """Transcrit un segment PCM float32 mono.

        `audio` doit etre normalise dans [-1, 1] a 16 kHz — c'est ce
        qu'attend Whisper.
        """
        if audio.dtype != np.float32:
            audio = audio.astype(np.float32)

        if taux != 16000:
            # Reechantillonnage lineaire : suffisant ici, la qualite du
            # signal micro est le facteur limitant, pas l'interpolation.
            cible = int(len(audio) * 16000 / taux)
            audio = np.interp(
                np.linspace(0, len(audio), cible, endpoint=False),
                np.arange(len(audio)),
                audio,
            ).astype(np.float32)

        segments, _ = self._modele.transcribe(
            audio,
            language=self.langue,
            vad_filter=self._vad,
            beam_size=1,          # greedy : on privilegie la latence
            condition_on_previous_text=False,
        )
        return " ".join(s.text.strip() for s in segments).strip()
