"""Verifie le nettoyage du texte avant synthese vocale.

Cas tires de sorties reelles observees au bench : le modele a produit
"signale(e)", que le TTS prononcait "signale parenthese e parenthese".

    .venv\\Scripts\\python.exe -m bench.test_texte
"""

from __future__ import annotations

import sys
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

from src.texte import nettoyer_pour_tts  # noqa: E402

CAS = [
    # (intitule, entree, sortie attendue)
    ("cas reel du bench : ecriture inclusive",
     "avez-vous ete signale(e) pour une fraude ?",
     "avez-vous ete signale pour une fraude ?"),

    ("inclusif pluriel",
     "Les visiteur(s) sont attendu(e)s.",
     "Les visiteur sont attendus."),

    ("point median",
     "Chers·es visiteur·euses.",
     "Chers visiteur."),

    ("gras markdown",
     "**Papiers**, s'il vous plait.",
     "Papiers, s'il vous plait."),

    ("italique et titre",
     "## Controle\nVotre _passeport_.",
     "Controle\nVotre passeport."),

    ("lien et code",
     "Voir [le reglement](http://x.fr) et `article 4`.",
     "Voir le reglement et article 4."),

    ("emoji",
     "Acces refuse 🚫 circulez.",
     "Acces refuse circulez."),

    ("guillemets et tirets typographiques",
     "Il a dit « non » — c'est ferme…",
     "Il a dit \"non\" - c'est ferme..."),

    ("ponctuation francaise preservee",
     "Vraiment ? Bien sur !",
     "Vraiment ? Bien sur !"),

    ("texte deja propre : inchange",
     "Papiers. Presentez vos documents.",
     "Papiers. Presentez vos documents."),

    ("chaine vide",
     "",
     ""),
]


def main() -> int:
    echecs = 0
    for intitule, entree, attendu in CAS:
        obtenu = nettoyer_pour_tts(entree)
        if obtenu == attendu:
            print(f"  ok     {intitule}")
        else:
            echecs += 1
            print(f"  ECHEC  {intitule}")
            print(f"           entree  : {entree!r}")
            print(f"           attendu : {attendu!r}")
            print(f"           obtenu  : {obtenu!r}")

    print()
    if echecs:
        print(f"  {echecs} cas en echec sur {len(CAS)}")
        return 1
    print(f"  {len(CAS)} cas valides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
