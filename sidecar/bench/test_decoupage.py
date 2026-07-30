"""Verifie le decoupage en phrases et l'extraction des tags.

Cette logique est le point delicat du streaming : si une phrase est emise
trop tot on coupe la parole, trop tard on perd le benefice du streaming, et
si un tag fuit dans le texte parle l'agent prononce "crochet EMOTION".

Les jetons passent par le VRAI ClientLLM.phrases(), via un transport httpx
factice qui rejoue un flux SSE — l'ancienne version reimplantait la boucle
de streaming dans le test, et une regression dans phrases() serait passee
inapercue.

Ne demande aucun modele — executable immediatement :

    py -3.12 -m bench.test_decoupage
"""

from __future__ import annotations

import asyncio
import json
import re
import sys
from pathlib import Path

import httpx

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

from src.llm import ClientLLM, Replique  # noqa: E402


def _corps_sse(jetons: list[str]) -> bytes:
    """Fabrique la reponse SSE qu'enverrait llama.cpp server."""
    lignes = ["data: " + json.dumps({"content": j}) + "\n\n" for j in jetons]
    lignes.append("data: " + json.dumps({"content": "", "stop": True}) + "\n\n")
    return "".join(lignes).encode()


async def _rejouer(jetons: list[str]) -> tuple[list[str], Replique | None, int]:
    """Passe les jetons dans ClientLLM.phrases() et collecte ce qui sort."""
    corps = _corps_sse(jetons)
    transport = httpx.MockTransport(lambda requete: httpx.Response(200, content=corps))

    # Les vraies grammaires du depot : le constructeur exige desormais les
    # trois cles, et c'est autant de verification gratuite qu'elles se
    # chargent sans erreur.
    client = ClientLLM({
        "url": "http://faux",
        "grammaires": {
            "entretien": "grammars/entretien.gbnf",
            "verdict": "grammars/verdict.gbnf",
            "cloture": "grammars/cloture.gbnf",
        },
    }, RACINE)
    await client._client.aclose()
    client._client = httpx.AsyncClient(transport=transport)

    appels_premier_jeton = 0

    def au_premier_jeton() -> None:
        nonlocal appels_premier_jeton
        appels_premier_jeton += 1

    phrases: list[str] = []
    finale: Replique | None = None
    try:
        async for phrase, replique in client.phrases(
            "prompt", "entretien", au_premier_jeton=au_premier_jeton
        ):
            if replique is not None:
                finale = replique
            elif phrase:
                phrases.append(phrase)
    finally:
        await client.fermer()

    return phrases, finale, appels_premier_jeton


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
    (
        # Les grammaires collent [EMOTION: a la ponctuation, sans espace :
        # les jetons de test doivent faire pareil, sinon ils testent un flux
        # que le modele ne produit jamais — c'est ainsi qu'une retention de
        # la derniere phrase est passee sous les radars.
        "phrase unique collee aux tags — emise des le crochet",
        ["Ou", " allez", "-vous", " ?", "[EMOTION:Stare][VERDICT:EN_COURS]"],
        1, "Stare", "EN_COURS",
    ),
    (
        "decimale : « 2.5 » ne doit pas couper la phrase",
        ["Vous", " restez", " 2", ".", "5", " jours", " ?", " Repondez", ".",
         "[EMOTION:Concerned][VERDICT:EN_COURS]"],
        2, "Concerned", "EN_COURS",
    ),
    (
        "civilite : « M. Dupont » ne doit pas couper la phrase",
        ["Vos", " papiers", ",", " M", ".", " Dupont", ".",
         "[EMOTION:Neutral][VERDICT:EN_COURS]"],
        1, "Neutral", "EN_COURS",
    ),
]


def main() -> int:
    echecs = 0

    for intitule, jetons, nb_attendu, emo_attendue, verdict_attendu in CAS:
        phrases, finale, appels = asyncio.run(_rejouer(jetons))

        erreurs = []
        if len(phrases) != nb_attendu:
            erreurs.append(f"{len(phrases)} phrase(s) au lieu de {nb_attendu}")
        if finale is None:
            erreurs.append("aucune Replique finale emise")
        else:
            if finale.emotion != emo_attendue:
                erreurs.append(f"emotion {finale.emotion!r} au lieu de {emo_attendue!r}")
            if finale.verdict != verdict_attendu:
                erreurs.append(f"verdict {finale.verdict!r} au lieu de {verdict_attendu!r}")
            if re.search(r"\[(EMOTION|VERDICT)", finale.texte):
                erreurs.append("un tag subsiste dans le texte nettoye")
        if any("[" in p for p in phrases):
            erreurs.append("un tag a fuit dans le texte parle")
        if appels != 1:
            erreurs.append(f"au_premier_jeton appele {appels} fois au lieu de 1")

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
            if finale is not None:
                print(f"           -> {finale.emotion} / {finale.verdict}")

    print()
    if echecs:
        print(f"  {echecs} cas en echec sur {len(CAS)}")
        return 1
    print(f"  {len(CAS)} cas valides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
