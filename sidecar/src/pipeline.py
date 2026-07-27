"""Orchestration STT -> LLM -> TTS, en streaming.

Le principe qui fait tout le gain : on ne serialise pas les etages. Des
qu'une phrase sort du LLM, elle part au TTS pendant que le modele ecrit la
suite. Le premier son tombe donc bien avant que la reponse soit complete.

La machine a etats reprend le Narrative Design Convai
(voir docs/NARRATIVE-DESIGN.md) :

    INTRO -> Interrogatoire (5 a 10 questions) -> accepte | refus

Le plancher et le plafond de questions ne dependent pas du bon vouloir du
modele : ils sont imposes par bascule de grammaire GBNF.
"""

from __future__ import annotations

from collections.abc import AsyncIterator
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import yaml

from .llm import ClientLLM, Replique
from .machine_etats import Phase, SessionEtat
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

        chemin = racine / config["scenario"]["fichier"]
        self.scenario = yaml.safe_load(chemin.read_text(encoding="utf-8"))

        interro = self.scenario["interrogatoire"]
        self.etat = SessionEtat(
            questions_min=interro.get("questions_min", 5),
            questions_max=interro.get("questions_max", 10),
        )
        self.historique: list[tuple[str, str]] = []

    def reinitialiser(self) -> None:
        """Remet la session a zero — appele quand un visiteur quitte la zone."""
        self.etat.reinitialiser()
        self.historique.clear()

    # -- Construction du prompt -----------------------------------------

    def _consignes(self) -> str:
        """Assemble les consignes selon la phase courante."""
        s = self.scenario
        e = self.etat
        if e.phase is Phase.INTRO:
            return s["intro"].strip()

        objectif = s["interrogatoire"]["objectif"].strip()
        restant = e.questions_max - e.nb_questions

        if e.nb_questions < e.questions_min:
            manquantes = e.questions_min - e.nb_questions
            return (
                f"{objectif}\n\n"
                f"Questions posees : {e.nb_questions}. "
                f"Il en reste au minimum {manquantes} avant tout verdict."
            )
        if e.nb_questions >= e.questions_max:
            return (
                f"{objectif}\n\n"
                f"Tu as pose {e.nb_questions} questions. "
                f"Tu dois MAINTENANT rendre ton verdict.\n\n"
                f"Si tu accordes l'acces :\n{s['verdicts']['accepte']['objectif'].strip()}\n\n"
                f"Si tu refuses l'acces :\n{s['verdicts']['refus']['objectif'].strip()}"
            )
        return (
            f"{objectif}\n\n"
            f"Questions posees : {e.nb_questions}. "
            f"Tu peux poursuivre ({restant} au maximum) ou rendre ton verdict."
        )

    def _construire_prompt(self, entree_visiteur: str) -> str:
        """Assemble le prompt au format ChatML (Qwen)."""
        parties = [
            "<|im_start|>system\n",
            self.scenario["persona"].strip(),
            "\n\n",
            self._consignes(),
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

    # -- Tour de parole ---------------------------------------------------

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
            yield Replique(
                texte=self.scenario["repli"]["incompris"],
                emotion="Concerned",
                verdict="EN_COURS",
            )
            return

        # --- 2 & 3. Generation et synthese, entrelacees ------------------
        prompt = self._construire_prompt(texte_visiteur)
        grammaire = self.etat.grammaire
        premier = True
        finale: Replique | None = None

        async for phrase, replique in self.llm.phrases(prompt, grammaire):
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

        if finale is None:
            return

        # --- 4. Avancement de la machine a etats -------------------------
        # Sur ACCEPTE/REFUSE, la suite — tampon puis invitation a liberer la
        # zone — est geree cote Unreal, comme elle l'a toujours ete.
        self.historique.append((texte_visiteur, finale.texte))
        self.etat.avancer(finale.verdict)

        yield finale

    async def fermer(self) -> None:
        await self.llm.fermer()
