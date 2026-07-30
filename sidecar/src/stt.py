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
import threading

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
        # Un seul acces natif a la fois. Le wait_for du pipeline abandonne
        # l'ATTENTE, pas le thread : apres un depassement de delai, le thread
        # abandonne est peut-etre encore dans transcribe() ou en plein
        # rechargement de self._modele. Sans ce verrou, le tour suivant
        # lancait une seconde inference sur le meme modele pendant que le
        # premier thread le remplacait et liberait l'ancien — use-after-free
        # dans ctranslate2, crash dur du sidecar. L'attente du verrou est
        # bornee (ATTENTE_VERROU) : voir transcrire().
        self._verrou = threading.Lock()
        # Epoque du couple (verrou, modele). Quand un thread gele detient
        # le verrou pour de bon, on abandonne verrou ET modele au thread et
        # on repart a neuf ; l'epoque empeche le naufrage de recharger son
        # vieux monde par-dessus le nouveau quand il finit par se reveiller.
        self._epoque = 0

        # Meme philosophie au DEMARRAGE qu'a l'execution : « une borne lente
        # vaut mieux qu'une borne sourde ». Un poste sans pile CUDA complete
        # (cuBLAS/cuDNN manquants — cas reel d'une reinstallation) plantait
        # ici au lieu de demarrer sur CPU.
        try:
            self._modele = self._charger()
        except Exception:
            if self._peripherique == "cpu":
                raise
            _log.exception(
                "chargement STT impossible sur %s — demarrage sur CPU",
                self._peripherique,
            )
            self._peripherique = "cpu"
            self._type_calcul = "int8"
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

    # Attente maximale du verrou d'inference — voir transcrire().
    ATTENTE_VERROU = 2.0

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

        # Attente BORNEE. Le verrou n'est disputable que si une inference
        # abandonnee par le wait_for du pipeline tourne encore — le serveur
        # serialise deja les tours. Attendre sans limite empilait un thread
        # de l'executor partage par enonce derriere une inference gelee,
        # jusqu'a l'assecher : plus aucun to_thread ne partait, TTS et
        # repliques de secours compris — borne muette, sans crash ni trace.
        if not self._verrou.acquire(timeout=self.ATTENTE_VERROU):
            # Ces echecs COMPTENT : sans cela, un verrou tenu pour toujours
            # (thread gele dans ctranslate2) rendait toute la mecanique de
            # recuperation inatteignable — « Repetez. » a chaque enonce,
            # pour toujours, sans une ligne au-dela du warning.
            self._echecs_inference += 1
            if (self._peripherique != "cpu"
                    and self._echecs_inference >= self.ECHECS_AVANT_CPU):
                _log.error(
                    "verrou STT tenu depuis %d enonces — thread gele "
                    "abandonne, on repart a neuf sur CPU",
                    self._echecs_inference,
                )
                self._epoque += 1
                self._verrou = threading.Lock()
                self._echecs_inference = 0
                self._basculer_cpu()
            # RuntimeError, pas TimeoutError : le cas courant est un
            # rechargement de quelques secondes encore en cours, qui se
            # resorbe seul. Faire repeter le visiteur (« Repetez. »)
            # suffit ; « Poste ferme. Repassez plus tard. » le chassait a
            # l'instant meme ou le modele redevenait sain.
            raise RuntimeError("inference STT precedente encore en cours")
        try:
            return self._transcrire_verrouille(audio, taux)
        finally:
            self._verrou.release()

    def _transcrire_verrouille(self, audio: np.ndarray, taux: int) -> str:
        epoque = self._epoque
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
            if epoque == self._epoque:
                self._echecs_inference = 0
            return texte
        except Exception:
            if epoque != self._epoque:
                # Le monde a change pendant qu'on tournait : verrou et
                # modele ont ete remplaces (bascule CPU pour verrou tenu).
                # Surtout ne pas recharger notre vieux monde par-dessus.
                raise
            self._echecs_inference += 1
            # Le tour courant est perdu — le repli d'en haut s'en charge —
            # mais le SUIVANT doit trouver un modele en etat de marche.
            _log.exception("inference STT tombee sur %s — rechargement", self._peripherique)
            self._recharger()
            raise
