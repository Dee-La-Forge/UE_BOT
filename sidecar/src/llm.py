"""Client LLM — streaming token par token vers llama.cpp server.

Le LLM tourne dans un processus separe (binaire CUDA precompile). On lui
parle en HTTP/SSE sur la boucle locale : cout < 1 ms, et si le modele
plante, le sidecar survit.

Point cle du prototype : on n'attend PAS la reponse complete. Des qu'une
phrase est terminee, elle part au TTS pendant que le modele ecrit la suite.
C'est de la que vient le gain de latence — pas du fait d'etre "local".
"""

from __future__ import annotations

import json
import re
from collections.abc import AsyncIterator
from dataclasses import dataclass
from pathlib import Path

import httpx

# Fin de phrase : ponctuation forte suivie d'un espace ou de la fin du flux.
_FIN_DE_PHRASE = re.compile(r"[.!?…]+[\s»\"']*")

# Tags de fin de replique, non destines a etre prononces.
_TAG_EMOTION = re.compile(r"\[EMOTION:(\w+)\]")
_TAG_VERDICT = re.compile(r"\[VERDICT:(\w+)\]")


@dataclass
class Replique:
    """Reponse complete de l'agent, une fois le flux termine."""

    texte: str
    emotion: str = "Neutral"
    verdict: str = "EN_COURS"


class ClientLLM:
    def __init__(self, config: dict, racine: Path):
        self.url = config["url"].rstrip("/")
        self.temperature = config.get("temperature", 0.7)
        self.top_p = config.get("top_p", 0.9)
        self.max_tokens = config.get("max_tokens", 200)

        self._grammaire = None
        chemin_grammaire = config.get("grammaire")
        if chemin_grammaire:
            fichier = racine / chemin_grammaire
            if fichier.exists():
                # On retire les commentaires : llama.cpp ne les accepte pas.
                brut = fichier.read_text(encoding="utf-8")
                self._grammaire = "\n".join(
                    ligne for ligne in brut.splitlines()
                    if not ligne.lstrip().startswith("#")
                ).strip()

        self._client = httpx.AsyncClient(timeout=httpx.Timeout(60.0, connect=5.0))

    async def disponible(self) -> bool:
        """Verifie que llama.cpp server repond, avant de lancer une session."""
        try:
            r = await self._client.get(f"{self.url}/health", timeout=2.0)
            return r.status_code == 200
        except (httpx.HTTPError, OSError):
            return False

    async def phrases(self, prompt: str) -> AsyncIterator[tuple[str, Replique | None]]:
        """Emet chaque phrase des qu'elle est complete.

        Produit des tuples (phrase, None) au fil de l'eau, puis un dernier
        ("", Replique) portant le texte complet et les tags extraits.
        """
        charge = {
            "prompt": prompt,
            "temperature": self.temperature,
            "top_p": self.top_p,
            "n_predict": self.max_tokens,
            "stream": True,
            "cache_prompt": True,  # reutilise le prefixe systeme entre les tours
        }
        if self._grammaire:
            charge["grammar"] = self._grammaire

        complet = ""      # tout ce que le modele a produit, tags compris
        tampon = ""       # texte parle en attente d'une fin de phrase
        dans_les_tags = False

        async with self._client.stream("POST", f"{self.url}/completion", json=charge) as reponse:
            reponse.raise_for_status()
            async for ligne in reponse.aiter_lines():
                if not ligne.startswith("data: "):
                    continue
                try:
                    evenement = json.loads(ligne[6:])
                except json.JSONDecodeError:
                    continue

                morceau = evenement.get("content", "")
                if morceau:
                    complet += morceau

                    # Des le premier crochet, on quitte la zone parlee.
                    if not dans_les_tags:
                        if "[" in morceau:
                            avant, _, _ = morceau.partition("[")
                            tampon += avant
                            dans_les_tags = True
                        else:
                            tampon += morceau

                        # Emettre toutes les phrases completes du tampon.
                        while (m := _FIN_DE_PHRASE.search(tampon)) is not None:
                            coupe = m.end()
                            phrase = tampon[:coupe].strip()
                            tampon = tampon[coupe:]
                            if phrase:
                                yield phrase, None

                if evenement.get("stop"):
                    break

        # Reste eventuel sans ponctuation finale.
        reste = tampon.strip()
        if reste:
            yield reste, None

        yield "", self._extraire(complet)

    @staticmethod
    def _extraire(brut: str) -> Replique:
        """Separe le texte parle des tags de controle."""
        emotion = m.group(1) if (m := _TAG_EMOTION.search(brut)) else "Neutral"
        verdict = m.group(1) if (m := _TAG_VERDICT.search(brut)) else "EN_COURS"
        texte = _TAG_VERDICT.sub("", _TAG_EMOTION.sub("", brut)).strip()
        return Replique(texte=texte, emotion=emotion, verdict=verdict)

    async def fermer(self) -> None:
        await self._client.aclose()
