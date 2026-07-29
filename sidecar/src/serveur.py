"""Serveur WebSocket — le pont vers Unreal.

Implemente le contrat decrit dans docs/CONTRAT-EVENEMENTS.md.

Principe : **le sidecar ignore tout de la scenographie.** Il ne sait pas ce
qu'est un glitch, un tampon ou un panneau de sortie. Il emet des faits ;
Unreal decide de la mise en scene. On peut ainsi retoucher les effets sans
toucher a l'IA, et changer de LLM sans rouvrir un Blueprint.

Les trames audio passent en binaire, precedees du JSON qui les decrit :
l'encodage base64 couterait ~33 % de volume sur un chemin ou chaque
milliseconde compte.
"""

from __future__ import annotations

import asyncio
import json
import logging
from pathlib import Path

import numpy as np
import websockets
from websockets.asyncio.server import ServerConnection, serve

from .metrics import Mesure
from .pipeline import MorceauAudio, Pipeline

_log = logging.getLogger("sidecar.serveur")

TAUX_VISITEUR = 16000   # ce qu'attend Whisper


class Serveur:
    def __init__(self, config: dict, racine: Path):
        self.config = config
        self.racine = racine
        self.pipeline = Pipeline(config, racine)

        self._repli = self.pipeline.scenario["repli"]
        self._avatar = self.pipeline.scenario.get("metahuman", "BP_AgentGermain")
        self._journal = racine / config["mesures"]["fichier_sortie"]
        self._mesures_actives = bool(config["mesures"]["actif"])

        # Un seul visiteur a la fois : la borne est mono-poste.
        self._occupe = asyncio.Lock()

        # Une seule replique a la fois sur la socket. L'intro tourne en tache
        # parallele : sans ce verrou, un visiteur qui parle pendant l'accueil
        # (echo du haut-parleur capte par le VAD, ou vrai barge-in) faisait
        # streamer DEUX repliques entrelacees — descripteurs JSON apparies
        # aux mauvaises trames binaires cote Unreal, et historique mute en
        # concurrence par deux _generer.
        self._parole = asyncio.Lock()

        self._tache_intro: asyncio.Task | None = None

    # -- Emission ---------------------------------------------------------

    @staticmethod
    async def _envoyer(ws: ServerConnection, evenement: str, **charge) -> None:
        await ws.send(json.dumps({"evenement": evenement, **charge}))

    async def _envoyer_audio(self, ws: ServerConnection, m: MorceauAudio, seq: int) -> None:
        """JSON descriptif, puis la trame binaire qu'il annonce."""
        # Plus de frise de visemes : elle nommait des poses MHF_* que
        # Convai_MetaHuman_FaceAnim n'expose pas. Unreal les recevait et les
        # ecrivait dans le vide. Le lipsync viendra d'Audio2Face, par
        # LiveLink, sans passer par ce canal.
        await self._envoyer(
            ws, "parole.audio",
            seq=seq, taux=m.taux, texte=m.texte, premier=m.premier,
        )
        await ws.send(self.pipeline.tts.en_pcm16(m.pcm))

    async def _replique_de_repli(self, ws: ServerConnection, cle: str) -> None:
        """Fait parler l'agent malgre une panne, plutot que de le laisser muet."""
        texte = self._repli.get(cle, self._repli.get("indisponible", ""))
        if not texte:
            return
        try:
            # to_thread : Piper est synchrone, et le laisser dans l'event
            # loop rendrait le sidecar sourd pendant la synthese.
            parole = await asyncio.to_thread(self.pipeline.tts.synthetiser, texte)
            await self._envoyer(ws, "parole.debut", texte=texte, emotion="Neutral")
            await self._envoyer_audio(
                ws,
                MorceauAudio(pcm=parole.pcm, taux=parole.taux, texte=texte,
                             premier=True),
                seq=0,
            )
            await self._envoyer(ws, "parole.fin")
        except Exception:
            # Le TTS lui-meme est tombe : on previent, Unreal affichera
            # son propre repli. Une borne muette reste preferable a une
            # borne gelee.
            _log.exception("repli TTS indisponible")
            await self._envoyer(ws, "erreur", code="tts_indisponible", repli=texte)

    # -- Tour de parole ---------------------------------------------------

    def _annuler_intro(self) -> None:
        """Coupe l'intro si elle court encore."""
        tache = getattr(self, "_tache_intro", None)
        if tache is not None and not tache.done():
            tache.cancel()
        self._tache_intro = None

    async def _jouer_intro(self, ws: ServerConnection) -> None:
        """Fait parler l'agent des l'arrivee du visiteur.

        Sans cette amorce la borne restait muette : le pipeline n'avait
        d'entree que par l'audio du visiteur, et un visiteur ne s'adresse pas
        spontanement a un garde-frontiere silencieux. Cote Unreal la session
        demarrait bien, le glitch se jouait, l'avatar changeait — et personne
        ne disait rien.

        Le repli n'est tente que si RIEN n'a ete prononce. Une panne survenue
        en cours de replique ferait sinon parler l'agent deux fois.
        """
        seq = 0
        a_parle = False

        # Sous le verrou de parole : un enonce du visiteur arrivant pendant
        # l'accueil attendra la fin de l'intro au lieu de s'y entrelacer.
        async with self._parole:
            try:
                async for element in self.pipeline.intro():
                    if isinstance(element, MorceauAudio):
                        if not a_parle:
                            await self._envoyer(ws, "parole.debut", texte=element.texte,
                                                emotion=self._emotion_pressentie())
                            a_parle = True
                        await self._envoyer_audio(ws, element, seq)
                        seq += 1
                    else:
                        if a_parle:
                            await self._envoyer(ws, "parole.fin")
                        await self._conclure(ws, element)

            except asyncio.CancelledError:
                # Le visiteur est parti pendant l'accueil : on se tait, sans
                # repli. Parler a une zone vide serait pire que le silence.
                _log.info("intro interrompue — visiteur parti")
                raise

            except Exception:
                _log.exception("echec de l'intro")

            if not a_parle:
                _log.warning("intro muette — repli sur la replique d'accueil")
                await self._replique_de_repli(ws, "accueil")

    async def _traiter_audio(self, ws: ServerConnection, brut: bytes) -> None:
        audio = np.frombuffer(brut, dtype=np.int16).astype(np.float32) / 32768.0
        if audio.size == 0:
            return

        # Sous le meme verrou que l'intro : deux repliques ne doivent jamais
        # streamer en meme temps sur la meme socket.
        async with self._parole:
            # Verdict rendu : l'entretien est clos. Repondre encore ferait
            # produire un nouveau verdict a chaque parole du visiteur, et la
            # borne oscillerait entre Verdict et SortieZone aussi longtemps
            # qu'il parlerait. Unreal pose deja ce garde ; on le double ici,
            # parce qu'une borne sans surveillance ne se rattrape pas.
            # (Teste sous le verrou : l'intro ou le tour precedent peut avoir
            # change l'etat pendant qu'on attendait.)
            if self.pipeline.etat.terminee:
                _log.info("parole ignoree : entretien deja clos")
                return

            mesure = Mesure()
            seq = 0
            a_parle = False

            try:
                async for element in self.pipeline.tour_de_parole(
                    audio, TAUX_VISITEUR, mesure
                ):
                    if isinstance(element, MorceauAudio):
                        if not a_parle:
                            await self._envoyer(ws, "parole.debut", texte=element.texte,
                                                emotion=self._emotion_pressentie())
                            a_parle = True
                        await self._envoyer_audio(ws, element, seq)
                        seq += 1
                    else:
                        if a_parle:
                            await self._envoyer(ws, "parole.fin")
                        await self._conclure(ws, element)

            except Exception:
                _log.exception("echec du tour de parole")
                await self._replique_de_repli(ws, "incompris")
                return

        if self._mesures_actives and mesure.temps_premier_son is not None:
            # I/O disque hors de l'event loop, comme les inferences.
            await asyncio.to_thread(mesure.ecrire, self._journal, contexte={
                "modele_llm": self.config["llm"]["modele"],
                "questions": self.pipeline.etat.nb_questions,
            })

    def _emotion_pressentie(self) -> str:
        """Emotion annoncee a l'ouverture de la replique.

        Le tag reel n'arrive qu'en fin de generation, or Unreal a besoin de
        poser le visage AVANT le premier son. On envoie donc le defaut du
        personnage, corrige ensuite si le tag differe.
        """
        return "Stare"

    async def _conclure(self, ws: ServerConnection, replique) -> None:
        """Emet l'emotion definitive, puis le verdict s'il y en a un."""
        await self._envoyer(ws, "emotion", valeur=replique.emotion)

        if replique.verdict in ("ACCEPTE", "REFUSE"):
            await self._envoyer(ws, "verdict", decision=replique.verdict)
            await self._envoyer(ws, "session.terminee")

    # -- Boucle de connexion ----------------------------------------------

    async def _connexion(self, ws: ServerConnection) -> None:
        _log.info("Unreal connecte")
        try:
            async for message in ws:
                if isinstance(message, bytes):
                    await self._traiter_audio(ws, message)
                    continue

                try:
                    recu = json.loads(message)
                except json.JSONDecodeError:
                    _log.warning("message illisible : %.80s", message)
                    continue

                await self._router(ws, recu.get("evenement", ""))

        except websockets.ConnectionClosed:
            _log.info("Unreal deconnecte")

        finally:
            # Unreal peut disparaitre SANS presence.perdue ni session.reset :
            # crash, arret de PIE, cable debranche. Sans ce nettoyage, _occupe
            # restait tenu pour toujours et chaque presence.detectee suivante
            # etait ignoree — plus aucune session ne demarrait jusqu'au
            # redemarrage manuel du sidecar. La borne est mono-poste : une
            # connexion qui tombe emporte la session, quelle qu'elle soit.
            self._annuler_intro()
            self.pipeline.reinitialiser()
            if self._occupe.locked():
                self._occupe.release()
            _log.info("connexion fermee — session liberee")

    async def _router(self, ws: ServerConnection, evenement: str) -> None:
        if evenement == "presence.detectee":
            if self._occupe.locked():
                _log.warning("presence signalee alors qu'une session court deja")
                return
            await self._occupe.acquire()
            self.pipeline.reinitialiser()
            await self._envoyer(ws, "session.demarree", avatar=self._avatar)
            _log.info("session demarree")

            # L'intro tourne A COTE de la boucle de reception, pas dedans.
            # L'attendre ici bloquerait `async for message in ws` pendant
            # toute la generation — LLM puis TTS, plusieurs secondes — et le
            # sidecar serait sourd au moment ou il parle : ni l'audio du
            # visiteur, ni son depart ne seraient traites.
            self._tache_intro = asyncio.create_task(self._jouer_intro(ws))

        elif evenement in ("presence.perdue", "session.reset"):
            # Le visiteur part : on coupe l'intro en cours plutot que de
            # continuer a parler dans le vide.
            self._annuler_intro()
            self.pipeline.reinitialiser()
            if self._occupe.locked():
                self._occupe.release()
            _log.info("session reinitialisee (%s)", evenement)

        else:
            _log.warning("evenement inconnu : %s", evenement)

    # -- Cycle de vie -----------------------------------------------------

    async def demarrer(self) -> None:
        hote = self.config["serveur"]["hote"]
        port = self.config["serveur"]["port"]

        if not await self.pipeline.llm.disponible():
            _log.warning(
                "llama.cpp injoignable sur %s — la borne demarre en mode degrade",
                self.pipeline.llm.url,
            )

        async with serve(self._connexion, hote, port, max_size=None):
            _log.info("sidecar a l'ecoute sur ws://%s:%d", hote, port)
            await asyncio.Future()   # tourne jusqu'a interruption
