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
import logging
import re
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from pathlib import Path

import httpx

_log = logging.getLogger("sidecar.llm")

# Fin de phrase : ponctuation forte, guillemets eventuels, puis un espace.
#
# L'espace final est EXIGE : l'ancien motif coupait « Vous restez 2.5
# jours ? » apres « 2. » et « M. Dupont » apres « M. » — le TTS recevait des
# fragments et la prosodie cassait au milieu d'un nombre. Le lookahead
# (?!\d) ecarte les decimales, les lookbehinds les civilites courantes.
# En flux, attendre l'espace revient a attendre le chunk suivant ; le reste
# sans ponctuation part de toute facon a la fin du flux.
_FIN_DE_PHRASE = re.compile(
    r"(?<!\bM)(?<!\bMme)(?<!\bDr)(?<!\bMlle)[.!?…]+(?!\d)[\s»\"']*\s"
)

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
    # Les trois phases du scenario ont chacune leur grammaire — ce sont les
    # cles que machine_etats.py distribue via SessionEtat.grammaire.
    GRAMMAIRES_REQUISES = ("entretien", "verdict", "cloture")

    def __init__(self, config: dict, racine: Path):
        self.url = config["url"].rstrip("/")
        self.temperature = config.get("temperature", 0.7)
        self.top_p = config.get("top_p", 0.9)
        self.max_tokens = config.get("max_tokens", 200)

        # Deux grammaires : l'entretien interdit grammaticalement le verdict,
        # le verdict l'autorise. La bascule est pilotee par pipeline.py.
        self._grammaires: dict[str, str] = {}
        for nom, chemin in (config.get("grammaires") or {}).items():
            fichier = racine / chemin
            if not fichier.exists():
                raise FileNotFoundError(f"Grammaire introuvable : {fichier}")
            # On retire les commentaires : llama.cpp ne les accepte pas.
            brut = fichier.read_text(encoding="utf-8")
            self._grammaires[nom] = "\n".join(
                ligne for ligne in brut.splitlines()
                if not ligne.lstrip().startswith("#")
            ).strip()

        # Verifie au CABLAGE, pas au premier tour : une faute de frappe
        # dans config.yaml ne se decouvrait qu'apres qu'un visiteur avait
        # deja recu une generation sans aucune contrainte — ni bornes de
        # questions, ni format de tags. Meme politique de refus au
        # demarrage que tts.phonemes_pour_repli ; le warning de phrases()
        # reste en filet.
        manquantes = [n for n in self.GRAMMAIRES_REQUISES
                      if n not in self._grammaires]
        if manquantes:
            raise RuntimeError(
                "grammaires manquantes dans llm.grammaires : "
                + ", ".join(manquantes)
            )

        # Le delai par defaut porte sur CHAQUE lecture, pas sur le total :
        # un llama.cpp sain streame un chunk toutes les quelques dizaines de
        # ms, et n_predict borne le total. 60 s laissaient donc un serveur
        # fige tenir le verrou de parole une minute entiere avant la panne
        # franche ; 20 s suffisent tres largement a distinguer « il
        # reflechit » de « il est mort ».
        self._client = httpx.AsyncClient(timeout=httpx.Timeout(20.0, connect=5.0))

    async def disponible(self) -> bool:
        """Verifie que llama.cpp server repond, avant de lancer une session."""
        try:
            r = await self._client.get(f"{self.url}/health", timeout=2.0)
            return r.status_code == 200
        except (httpx.HTTPError, OSError):
            return False

    async def phrases(
        self, prompt: str, grammaire: str = "entretien",
        au_premier_jeton: Callable[[], None] | None = None,
    ) -> AsyncIterator[tuple[str, Replique | None]]:
        """Emet chaque phrase des qu'elle est complete.

        `grammaire` selectionne le jeu de verdicts autorises : "entretien"
        interdit toute cloture, "verdict" la permet.

        `au_premier_jeton` est rappele a l'arrivee du premier contenu SSE :
        c'est le seul endroit qui VOIT le premier token. La mesure etait
        posee avant a la premiere phrase complete — « LLM (1er token) »
        mentait dans tous les releves, et « LLM (1re phrase) » valait 0.

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
        if (g := self._grammaires.get(grammaire)) is not None:
            charge["grammar"] = g
        else:
            # Une cle de grammaire absente (faute de frappe dans config.yaml)
            # generait SANS contrainte, sans un mot : plus de bornes de
            # questions, plus de format de tags garanti — et le premier
            # crochet venu de la prose coupait la parole pour de bon.
            _log.warning(
                "grammaire '%s' introuvable — generation SANS contrainte",
                grammaire,
            )

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
                    if au_premier_jeton is not None:
                        au_premier_jeton()
                        au_premier_jeton = None
                    complet += morceau

                    # Des le premier crochet, on quitte la zone parlee.
                    if not dans_les_tags:
                        if "[" in morceau:
                            avant, _, _ = morceau.partition("[")
                            tampon += avant
                            dans_les_tags = True

                            # Le texte parle est COMPLET : la derniere
                            # phrase part maintenant. Les grammaires collent
                            # [EMOTION: a la ponctuation, sans espace — or
                            # _FIN_DE_PHRASE exige un espace : sans cette
                            # purge, la derniere (souvent seule) phrase
                            # attendait la fin de la generation des tags, et
                            # le TTS ne recouvrait plus rien — regression
                            # silencieuse du premier son sur chaque tour a
                            # une phrase.
                            if (phrase := tampon.strip()):
                                yield phrase, None
                            tampon = ""
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
