"""Point d'entree du sidecar.

    .venv\\Scripts\\python.exe main.py

Prerequis : llama.cpp server lance separement (scripts/lancer_llm.ps1).
Le sidecar demarre meme s'il est absent — la borne bascule alors en mode
degrade plutot que de refuser de servir.
"""

from __future__ import annotations

import asyncio
import logging
import sys
from pathlib import Path

import yaml

RACINE = Path(__file__).resolve().parent
sys.path.insert(0, str(RACINE))

from src.serveur import Serveur  # noqa: E402


def configurer_journal(racine: Path) -> None:
    """Console + fichier rotatif : sur une borne autonome, le fichier est
    la seule trace exploitable apres coup."""
    from logging.handlers import RotatingFileHandler

    dossier = racine / "logs"
    dossier.mkdir(exist_ok=True)

    format_ = logging.Formatter(
        "%(asctime)s  %(levelname)-7s  %(name)-18s  %(message)s",
        datefmt="%H:%M:%S",
    )

    console = logging.StreamHandler()
    console.setFormatter(format_)

    fichier = RotatingFileHandler(
        dossier / "sidecar.log", maxBytes=5_000_000, backupCount=5, encoding="utf-8"
    )
    fichier.setFormatter(format_)

    logging.basicConfig(level=logging.INFO, handlers=[console, fichier])


async def principal() -> int:
    configurer_journal(RACINE)
    log = logging.getLogger("sidecar")

    config = yaml.safe_load((RACINE / "config.yaml").read_text(encoding="utf-8"))

    log.info("chargement des modeles...")
    try:
        serveur = Serveur(config, RACINE)
    except FileNotFoundError as e:
        log.error("modele manquant : %s", e)
        log.error("lancer scripts/telecharger.sh")
        return 1
    except RuntimeError as e:
        # Refus volontaire au demarrage : cle de config invalide
        # (grammaire manquante, fonctionnalite retiree reactivee).
        log.error("configuration invalide : %s", e)
        return 1

    log.info("STT %s/%s  |  TTS %s  |  LLM %s",
             config["stt"]["modele"], config["stt"]["peripherique"],
             config["tts"]["voix"], config["llm"]["modele"])

    try:
        await serveur.demarrer()
    finally:
        # Un Ctrl+C arrive ici en CancelledError sous asyncio.run — jamais
        # en KeyboardInterrupt. Le finally ferme les clients HTTP dans tous
        # les cas, arret propre ou non.
        await serveur.pipeline.fermer()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(principal()))
    except KeyboardInterrupt:
        sys.exit(0)
