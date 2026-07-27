"""Orchestration STT -> LLM -> TTS, en streaming.

Le principe qui fait tout le gain : on ne serialise pas les etages. Des
qu'une phrase sort du LLM, elle part au TTS pendant que le modele ecrit la
suite. Le premier son tombe donc bien avant que la reponse soit complete.
"""

from __future__ import annotations

from collections.abc import AsyncIterator
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import yaml

from .llm import ClientLLM, Replique
from .metrics import Mesure
from .stt import Transcripteur
from .tts import Synthetiseur


@dataclass
class MorceauAudio:
    """Un fragment de parole pret a etre joue et anime."""

    pcm: np.ndarray
    taux: int
    texte: str
    premier: bool


class Pipeline:
    def __init__(self, config: dict, racine: Path):
        self.config = config
        self.racine = racine

        self.stt = Transcripteur(config["stt"])
        self.llm = ClientLLM(config["llm"], racine)
        self.tts = Synthetiseur(config["tts"], racine)

        scenario_path = racine / config["scenario"]["fichier"]
        self.scenario = yaml.safe_load(scenario_path.read_text(encoding="utf-8"))

        self.historique: list[tuple[str, str]] = []

    def _construire_prompt(self, entree_visiteur: str) -> str:
        """Assemble le prompt au format ChatML (Qwen)."""
        parties = [
            "<|im_start|>system\n",
            self.scenario["persona"].strip(),
            "\n\nTermine TOUJOURS ta reponse par les deux tags, dans cet ordre :",
            "\n[EMOTION:...][VERDICT:...]",
            "\n<|im_end|>\n",
        ]
        for visiteur, agent in self.historique:
            parties.append(f"<|im_start|>user\n{visiteur}<|im_end|>\n")
            parties.append(f"<|im_start|>assistant\n{agent}<|im_end|>\n")
        parties.append(f"<|im_start|>user\n{entree_visiteur}<|im_end|>\n")
        parties.append("<|im_start|>assistant\n")
        return "".join(parties)

    async def tour_de_parole(
        self,
        audio_visiteur: np.ndarray,
        taux: int = 16000,
        mesure: Mesure | None = None,
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """Traite un tour complet : audio entrant -> audio sortant.

        Emet des MorceauAudio au fil de l'eau, puis une Replique finale
        portant l'emotion et le verdict.
        """
        m = mesure or Mesure()
        m.marquer("debut")

        # --- 1. Transcription -------------------------------------------
        texte_visiteur = self.stt.transcrire(audio_visiteur, taux)
        m.marquer("stt_fin")

        if not texte_visiteur:
            yield Replique(texte="", emotion="Concerned", verdict="EN_COURS")
            return

        # --- 2 & 3. Generation et synthese, entrelacees ------------------
        prompt = self._construire_prompt(texte_visiteur)
        premier = True
        finale: Replique | None = None

        async for phrase, replique in self.llm.phrases(prompt):
            if replique is not None:
                finale = replique
                break

            if premier:
                m.marquer("llm_premier_token")
                m.marquer("llm_premiere_phrase")

            pcm = self.tts.synthetiser(phrase)

            if premier:
                m.marquer("tts_premier_chunk")
                m.marquer("audio_premier_chunk")

            yield MorceauAudio(pcm=pcm, taux=self.tts.taux, texte=phrase, premier=premier)
            premier = False

        m.marquer("fin")

        if finale is not None:
            self.historique.append((texte_visiteur, finale.texte))
            yield finale

    async def fermer(self) -> None:
        await self.llm.fermer()
