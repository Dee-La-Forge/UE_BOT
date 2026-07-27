"""Verifie le decoupage en phrases et l'extraction des tags.

Cette logique est le point delicat du streaming : si une phrase est emise
trop tot on coupe la parole, trop tard on perd le benefice du streaming, et
si un tag fuit dans le texte parle l'agent prononce "crochet EMOTION".

Ne demande aucun modele — executable immediatement :

    py -3.12 -m bench.test_decoupage
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

from src.llm import _FIN_DE_PHRASE, ClientLLM  # noqa: E402


def flux_vers_phrases(jetons: list[str]) -> tuple[list[str], str]:
    """Rejoue la logique de streaming de ClientLLM.phrases()."""
    complet = ""
    tampon = ""
    dans_les_tags = False
    phrases: list[str] = []

    for jeton in jetons:
        complet += jeton
        if dans_les_tags:
            continue
        if "[" in jeton:
            avant, _, _ = jeton.partition("[")
            tampon += avant
            dans_les_tags = True
        else:
            tampon += jeton
        while (m := _FIN_DE_PHRASE.search(tampon)) is not None:
            phrase = tampon[: m.end()].strip()
            tampon = tampon[m.end():]
            if phrase:
                phrases.append(phrase)

    if (reste := tampon.strip()):
        phrases.append(reste)
    return phrases, complet


CAS = [
    (
        "replique nominale, trois phrases",
        ["Bonjour", ".", " Presentez", "-moi", " vos", " papiers", ".",
         " Motif", " du", " voyage", " ?",
         "[EMOTION", ":", "Concerned", "]", "[VERDICT", ":", "EN_COURS", "]"],
        3, "Concerned", "EN_COURS",
    ),
    (
        "phrase unique, verdict d'acceptation",
        ["C'est", " bon", ",", " passez", ".",
         "[EMOTION:Happy][VERDICT:ACCEPTE]"],
        1, "Happy", "ACCEPTE",
    ),
    (
        "refus avec points de suspension",
        ["Non", "…", " Vous", " ne", " passerez", " pas", " !",
         "[EMOTION:Angry][VERDICT:REFUSE]"],
        2, "Angry", "REFUSE",
    ),
    (
        "sans ponctuation finale — le reste doit sortir quand meme",
        ["Attendez", " ici", "[EMOTION:Stare][VERDICT:EN_COURS]"],
        1, "Stare", "EN_COURS",
    ),
    (
        "tags absents — valeurs par defaut",
        ["Bonjour", "."],
        1, "Neutral", "EN_COURS",
    ),
]


def main() -> int:
    echecs = 0

    for intitule, jetons, nb_attendu, emo_attendue, verdict_attendu in CAS:
        phrases, complet = flux_vers_phrases(jetons)
        replique = ClientLLM._extraire(complet)

        erreurs = []
        if len(phrases) != nb_attendu:
            erreurs.append(f"{len(phrases)} phrase(s) au lieu de {nb_attendu}")
        if replique.emotion != emo_attendue:
            erreurs.append(f"emotion {replique.emotion!r} au lieu de {emo_attendue!r}")
        if replique.verdict != verdict_attendu:
            erreurs.append(f"verdict {replique.verdict!r} au lieu de {verdict_attendu!r}")
        if any("[" in p for p in phrases):
            erreurs.append("un tag a fuit dans le texte parle")
        if re.search(r"\[(EMOTION|VERDICT)", replique.texte):
            erreurs.append("un tag subsiste dans le texte nettoye")

        if erreurs:
            echecs += 1
            print(f"  ECHEC  {intitule}")
            for e in erreurs:
                print(f"         - {e}")
            print(f"         phrases : {phrases}")
        else:
            print(f"  ok     {intitule}")
            for i, p in enumerate(phrases, 1):
                marque = "1er son" if i == 1 else "       "
                print(f"           [{marque}] {p}")
            print(f"           -> {replique.emotion} / {replique.verdict}")

    print()
    if echecs:
        print(f"  {echecs} cas en echec sur {len(CAS)}")
        return 1
    print(f"  {len(CAS)} cas valides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
