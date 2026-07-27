"""Verifie que le plancher et le plafond de questions sont infaillibles.

Le Narrative Design impose "entre 5 et 10 questions, jamais moins de 5".
Convai s'en remettait au respect de la consigne par le modele. Ici la regle
passe par la grammaire GBNF : avant le plancher, le jeton de verdict n'est
pas productible ; au plafond, EN_COURS ne l'est plus.

Ne demande aucun modele — executable immediatement :

    py -3.12 -m bench.test_machine_etats
"""

from __future__ import annotations

import sys
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

from src.machine_etats import Phase, SessionEtat  # noqa: E402

echecs = 0


def verifier(intitule: str, condition: bool, detail: str = "") -> None:
    global echecs
    if condition:
        print(f"  ok     {intitule}")
    else:
        echecs += 1
        print(f"  ECHEC  {intitule}" + (f" — {detail}" if detail else ""))


def main() -> int:
    # --- Deroule nominal : INTRO puis 5 questions ------------------------
    e = SessionEtat(questions_min=5, questions_max=10)

    verifier("depart en phase INTRO", e.phase is Phase.INTRO)
    verifier("grammaire initiale = entretien", e.grammaire == "entretien")

    e.avancer("EN_COURS")  # l'INTRO ne compte pas comme une question
    verifier("apres INTRO -> INTERROGATOIRE", e.phase is Phase.INTERROGATOIRE)
    verifier("l'INTRO n'incremente pas le compteur", e.nb_questions == 0,
             f"nb_questions={e.nb_questions}")

    for i in range(1, 5):
        e.avancer("EN_COURS")
        verifier(
            f"question {i} : verdict encore interdit",
            e.grammaire == "entretien",
            f"grammaire={e.grammaire}",
        )

    e.avancer("EN_COURS")  # 5e question
    verifier("plancher atteint (5) : verdict autorise", e.grammaire == "verdict",
             f"nb_questions={e.nb_questions}, grammaire={e.grammaire}")

    # --- Plafond ---------------------------------------------------------
    while e.nb_questions < 10:
        e.avancer("EN_COURS")
    verifier("plafond atteint (10) : verdict impose", e.grammaire == "cloture",
             f"nb_questions={e.nb_questions}, grammaire={e.grammaire}")

    # --- Cloture ---------------------------------------------------------
    e.avancer("ACCEPTE")
    verifier("ACCEPTE termine la session", e.terminee)

    e2 = SessionEtat(5, 10)
    for _ in range(6):
        e2.avancer("EN_COURS")
    e2.avancer("REFUSE")
    verifier("REFUSE termine la session", e2.terminee)

    # --- Reinitialisation (sortie de zone LiDAR) -------------------------
    e2.reinitialiser()
    verifier("reinitialisation : retour en INTRO", e2.phase is Phase.INTRO)
    verifier("reinitialisation : compteur remis a zero", e2.nb_questions == 0)
    verifier("reinitialisation : grammaire redevient entretien",
             e2.grammaire == "entretien")

    # --- Garde-fous de configuration -------------------------------------
    try:
        SessionEtat(questions_min=8, questions_max=3)
        verifier("min > max rejete", False, "aucune exception levee")
    except ValueError:
        verifier("min > max rejete", True)

    try:
        SessionEtat(questions_min=0, questions_max=10)
        verifier("min = 0 rejete", False, "aucune exception levee")
    except ValueError:
        verifier("min = 0 rejete", True)

    print()
    if echecs:
        print(f"  {echecs} verification(s) en echec")
        return 1
    print("  toutes les verifications passent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
