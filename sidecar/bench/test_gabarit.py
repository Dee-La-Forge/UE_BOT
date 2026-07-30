"""Verifie les deux gabarits de prompt — ChatML et Mistral.

Le gabarit ne leve jamais d'erreur quand il est faux : le modele repond
simplement a cote (releve en essai reel — l'agent jouait le role du
visiteur). Ces cas verifient donc la STRUCTURE, seule chose qu'on puisse
tenir sans modele : balises attendues, historique restitue dans l'ordre,
et surtout PREFIXE STATIQUE — le cache de prompt de llama.cpp vaut 450 ms
par tour, il tombe des que le debut du prompt bouge.

Aucun modele requis :

    py -3.12 -m bench.test_gabarit
"""

from __future__ import annotations

import sys
import types
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

sys.modules.setdefault("faster_whisper", types.SimpleNamespace(WhisperModel=object))
sys.modules.setdefault("piper", types.SimpleNamespace(PiperVoice=object, SynthesisConfig=object))

import src.pipeline as module_pipeline  # noqa: E402


def fabriquer(gabarit: str):
    """Un Pipeline sans aucun modele : seul le prompt nous interesse."""
    p = object.__new__(module_pipeline.Pipeline)
    p._gabarit = gabarit
    p.historique = []
    p.scenario = {
        "persona": "Tu es un garde-frontiere.",
        "langue": "Tu parles francais.",
        "intro": "Tu interpelles le visiteur.",
        "interrogatoire": {
            "objectif": "Tu etablis son identite.",
            "sujets": ["identite", "motif"],
        },
        "verdicts": {"accepte": {"objectif": "A"}, "refus": {"objectif": "R"}},
    }
    p.etat = module_pipeline.SessionEtat(questions_min=2, questions_max=4)
    return p


CAS = []


def cas(nom):
    def enregistrer(f):
        CAS.append((nom, f))
        return f
    return enregistrer


@cas("chatml : balises et tour final ouvert")
def _():
    p = fabriquer("chatml")
    prompt = p._construire_prompt("Bonjour.")
    erreurs = []
    for balise in ("<|im_start|>system", "<|im_start|>user", "<|im_end|>"):
        if balise not in prompt:
            erreurs.append(f"balise absente : {balise}")
    if not prompt.endswith("<|im_start|>assistant\n"):
        erreurs.append("le prompt ne se termine pas sur un tour assistant ouvert")
    if "[INST]" in prompt:
        erreurs.append("du Mistral a fuit dans le ChatML")
    return erreurs


@cas("mistral : balises [INST] et tour final ouvert")
def _():
    p = fabriquer("mistral")
    prompt = p._construire_prompt("Bonjour.")
    erreurs = []
    if not prompt.startswith("<s>[INST] "):
        erreurs.append("le prompt ne commence pas par <s>[INST]")
    if not prompt.endswith(" [/INST]"):
        erreurs.append("le prompt ne se termine pas sur un tour ouvert")
    if "<|im_start|>" in prompt:
        erreurs.append("du ChatML a fuit dans le Mistral")
    return erreurs


@cas("mistral : l'historique est restitue dans l'ordre")
def _():
    p = fabriquer("mistral")
    p.etat.avancer("EN_COURS")
    p.historique = [("Je viens de Lyon.", "Vos papiers."),
                    ("Les voici.", "Motif du voyage ?")]
    prompt = p._construire_prompt("Le musee.")
    erreurs = []
    positions = [prompt.find(t) for t in
                 ("Je viens de Lyon.", "Vos papiers.", "Les voici.",
                  "Motif du voyage ?", "Le musee.")]
    if -1 in positions:
        erreurs.append("un tour de l'historique manque")
    elif positions != sorted(positions):
        erreurs.append("l'historique n'est pas dans l'ordre chronologique")
    if prompt.count("[/INST]") != 4:   # systeme + 2 tours + tour courant
        erreurs.append(f"{prompt.count('[/INST]')} blocs [/INST] au lieu de 4")
    return erreurs


@cas("PREFIXE STATIQUE : le debut ne bouge pas quand l'historique grandit")
def _():
    erreurs = []
    for gabarit in ("chatml", "mistral"):
        p = fabriquer(gabarit)
        vide = p._construire_prompt("Bonjour.")
        # Le prefixe est tout ce qui precede le premier tour de dialogue.
        marque = "<|im_start|>user" if gabarit == "chatml" else "[INST] Bonjour."
        prefixe = vide[:vide.find(marque)]

        p.etat.avancer("EN_COURS")
        p.historique = [("Je viens de Lyon.", "Vos papiers.")]
        charge = p._construire_prompt("Le musee.")

        if not charge.startswith(prefixe):
            erreurs.append(
                f"{gabarit} : le prefixe change avec l'historique — "
                "le cache de prompt tombe a chaque tour"
            )
    return erreurs


@cas("gabarit inconnu : refus au demarrage")
def _():
    config = {
        "stt": {}, "tts": {}, "scenario": {"fichier": "scenario/agent.yaml"},
        "llm": {"gabarit": "lama-3-inexistant", "url": "http://faux"},
    }
    try:
        module_pipeline.Pipeline(config, RACINE)
    except RuntimeError as e:
        return [] if "gabarit" in str(e) else [f"mauvaise erreur : {e}"]
    except Exception as e:
        return [f"refus attendu, obtenu {type(e).__name__} : {e}"]
    return ["un gabarit inconnu a ete accepte"]


def main() -> int:
    echecs = 0
    for nom, f in CAS:
        erreurs = f()
        if erreurs:
            echecs += 1
            print(f"  ECHEC  {nom}")
            for e in erreurs:
                print(f"         - {e}")
        else:
            print(f"  ok     {nom}")

    print()
    if echecs:
        print(f"  {echecs} cas en echec sur {len(CAS)}")
        return 1
    print(f"  {len(CAS)} cas valides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
