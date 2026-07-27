"""Machine a etats de la session — le coeur du scenario.

Volontairement sans aucune dependance lourde : ni modele, ni reseau, ni
audio. C'est la logique la plus critique du dispositif, elle doit rester
testable en une seconde et sans rien installer.

Calquee sur le Narrative Design Convai (docs/NARRATIVE-DESIGN.md) :

    INTRO -> Interrogatoire (5 a 10 questions) -> accepte | refus

La phase de sortie ("liberez la zone") n'est pas ici : elle etait deja
geree cote Unreal, et le reste.
"""

from __future__ import annotations

from enum import Enum


class Phase(Enum):
    INTRO = "intro"
    INTERROGATOIRE = "interrogatoire"
    TERMINE = "termine"


class SessionEtat:
    """Suit l'avancement d'un entretien et en deduit les contraintes."""

    def __init__(self, questions_min: int = 5, questions_max: int = 10):
        if questions_min < 1:
            raise ValueError("questions_min doit valoir au moins 1")
        if questions_max < questions_min:
            raise ValueError("questions_max doit etre >= questions_min")
        self.questions_min = questions_min
        self.questions_max = questions_max
        self.reinitialiser()

    def reinitialiser(self) -> None:
        """Remet a zero — appele quand un visiteur quitte la zone du LiDAR."""
        self.phase = Phase.INTRO
        self.nb_questions = 0

    @property
    def grammaire(self) -> str:
        """Traduit le compteur de questions en contrainte grammaticale.

        C'est ici que le "jamais moins de 5 questions" du Narrative Design
        cesse d'etre une consigne pour devenir une impossibilite : avant le
        plancher, le jeton de verdict n'est tout simplement pas productible.
        """
        if self.nb_questions < self.questions_min:
            return "entretien"   # verdict grammaticalement impossible
        if self.nb_questions >= self.questions_max:
            return "cloture"     # verdict grammaticalement obligatoire
        return "verdict"         # le modele decide

    @property
    def terminee(self) -> bool:
        return self.phase is Phase.TERMINE

    def avancer(self, verdict: str) -> None:
        """Fait progresser l'etat apres une replique de l'agent."""
        if self.phase is Phase.INTRO:
            self.phase = Phase.INTERROGATOIRE
        elif self.phase is Phase.INTERROGATOIRE:
            self.nb_questions += 1

        if verdict in ("ACCEPTE", "REFUSE"):
            self.phase = Phase.TERMINE
