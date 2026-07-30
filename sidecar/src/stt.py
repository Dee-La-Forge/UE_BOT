"""Transcription — faster-whisper.

Peripherique et precision viennent de config.yaml (`stt.peripherique`,
`stt.type_calcul`). L'endpointing (savoir quand le visiteur a fini de
parler) est deja resolu en amont par SileroVAD, cote Unreal. On ne recoit
donc que des segments utiles, ce qui evite de transcrire du silence.

Le GPU est partage avec Unreal et Audio2Face : une erreur CUDA (OOM, reset
du driver) n'est pas une hypothese d'ecole. Avant, le modele n'etait charge
qu'une fois et jamais recharge — le premier incident GPU rendait la borne
sourde pour toujours, chaque tour echouant a l'identique derriere le repli
« Repetez. ». On recharge desormais, et on retombe sur CPU si le GPU reste
inutilisable.
"""

from __future__ import annotations

import logging

import numpy as np
from faster_whisper import WhisperModel

_log = logging.getLogger("sidecar.stt")


class Transcripteur:
    def __init__(self, config: dict):
        self.langue = config.get("langue", "fr")
        self._vad = config.get("vad_filtre", True)
        self._nom = config.get("modele", "small")
        self._peripherique = config.get("peripherique", "cpu")
        self._type_calcul = config.get("type_calcul", "int8")
        # Amorce de domaine : orienter Whisper vers le vocabulaire du poste
        # frontiere coute zero latence et reduit les inventions sur les
        # fragments courts — la ou naissaient les « pandinvestigation ».
        self._contexte = config.get("contexte") or None
        # Echecs d'INFERENCE consecutifs. Le cas reel d'un GPU sature par
        # Unreal n'est pas « le chargement echoue » : les poids se chargent
        # tres bien, c'est l'activation qui manque de VRAM. Sans ce compteur,
        # chaque tour refaisait le meme cycle — echec d'inference,
        # rechargement CUDA reussi (plusieurs secondes), echec au tour
        # suivant — et la bascule CPU promise n'arrivait jamais.
        self._echecs_inference = 0
        self._modele = self._charger()

    def _charger(self) -> WhisperModel:
        return WhisperModel(
            self._nom,
            device=self._peripherique,
            compute_type=self._type_calcul,
        )

    # Au-dela de ce nombre d'echecs d'inference d'affilee, le GPU est
    # considere durablement inutilisable, meme si les chargements reussissent.
    ECHECS_AVANT_CPU = 3

    def _basculer_cpu(self) -> None:
        _log.warning("bascule definitive du STT sur CPU")
        self._peripherique = "cpu"
        self._type_calcul = "int8"
        self._modele = self._charger()

    def _recharger(self) -> None:
        """Tente de remettre le modele en etat apres une erreur d'inference.

        D'abord a l'identique — un CUDA OOM ponctuel se resorbe souvent au
        rechargement. Bascule sur CPU si le chargement echoue, OU si les
        inferences echouent en serie malgre des rechargements qui reussissent
        (VRAM durablement mangee par Unreal) : plus lent, mais une borne
        lente vaut mieux qu'une borne sourde.
        """
        if (self._peripherique != "cpu"
                and self._echecs_inference >= self.ECHECS_AVANT_CPU):
            _log.error(
                "%d echecs d'inference d'affilee sur %s — le rechargement "
                "ne suffit pas", self._echecs_inference, self._peripherique,
            )
            self._basculer_cpu()
            return

        try:
            self._modele = self._charger()
            _log.warning("STT recharge sur %s", self._peripherique)
        except Exception:
            if self._peripherique == "cpu":
                raise   # deja au plancher : rien d'autre a tenter
            _log.exception(
                "rechargement sur %s impossible — bascule definitive sur CPU",
                self._peripherique,
            )
            self._basculer_cpu()

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

        try:
            segments, _ = self._modele.transcribe(
                audio,
                language=self.langue,
                vad_filter=self._vad,
                beam_size=1,          # greedy : on privilegie la latence
                condition_on_previous_text=False,
                initial_prompt=self._contexte,
            )
            texte = " ".join(s.text.strip() for s in segments).strip()
            self._echecs_inference = 0
            return texte
        except Exception:
            self._echecs_inference += 1
            # Le tour courant est perdu — le repli d'en haut s'en charge —
            # mais le SUIVANT doit trouver un modele en etat de marche.
            _log.exception("inference STT tombee sur %s — rechargement", self._peripherique)
            self._recharger()
            raise
