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

import httpx
import numpy as np
import websockets
from websockets.asyncio.server import ServerConnection, serve

from .metrics import Mesure
from .pipeline import DELAI_TTS, MorceauAudio, Pipeline

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

        # Une seule relance a la fois : Unreal n'en envoie qu'une par
        # silence, mais une socket capricieuse ne doit pas pouvoir en
        # empiler. Suivie a part des tours pour que la borne de file
        # distingue « l'agent relance » d'« un enonce attend ».
        self._tache_relance: asyncio.Task | None = None

        # References fortes sur les fermetures d'anciennes sockets :
        # asyncio ne retient les taches que faiblement, et une fermeture
        # orpheline (le remplacant meurt avant qu'elle n'aboutisse) pouvait
        # etre detruite en plein vol — socket et descripteur fuites sur une
        # borne qui enchaine les redemarrages de PIE.
        self._taches_fermeture: set[asyncio.Task] = set()

        # La connexion Unreal courante. Une seule compte : quand Unreal
        # redemarre (arret de PIE, crash), la nouvelle socket s'etablit
        # parfois AVANT que l'ancienne ne meure (timeout TCP). Sans ce
        # repere, le nettoyage de l'ancienne detruisait la session de la
        # nouvelle — intro annulee, historique vide, verrou relache sous
        # les pieds d'une session bien vivante.
        self._ws_actif: ServerConnection | None = None

        # Tours de parole en cours ou en attente du verrou. Lances en taches
        # pour que la boucle de reception reste libre : avant, `await
        # _traiter_audio` la bloquait pendant tout un tour (STT + LLM + TTS,
        # plusieurs secondes) et le presence.perdue d'un visiteur parti
        # n'etait lu qu'apres — l'agent finissait sa replique dans le vide.
        self._taches_parole: set[asyncio.Task] = set()

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
        trame = self.pipeline.tts.en_pcm16(m.pcm)
        await ws.send(trame)
        _log.debug("trame %d envoyee : %d octets a %d Hz", seq, len(trame), m.taux)

    async def _dire(self, ws: ServerConnection, texte: str,
                    emotion: str = "Neutral") -> None:
        """Synthetise et streame une replique complete, hors pipeline LLM.

        Garantit qu'un parole.debut emis ici recoit TOUJOURS son parole.fin,
        panne ou annulation comprises : la synthese — le seul etage qui peut
        vraiment echouer — est faite AVANT le premier envoi, et la cloture
        vit dans un finally, que meme un CancelledError (BaseException, qui
        contournait le except de _jouer_flux) ne saute pas. Sans cela, un
        echec entre debut et fin laissait deux repliques ouvertes cote
        Unreal — et bRepliqueEnCours verrouille a vrai, micro sourd.
        """
        # to_thread : Piper est synchrone, et le laisser dans l'event loop
        # rendrait le sidecar sourd pendant la synthese. Borne comme les
        # etages du pipeline : un Piper fige ne doit pas gober le verrou.
        parole = await asyncio.wait_for(
            asyncio.to_thread(self.pipeline.tts.synthetiser, texte),
            timeout=DELAI_TTS,
        )
        # Journalise CE QUI PART REELLEMENT sur la socket. Sans cette
        # ligne, une replique de repli synthetisee et envoyee etait
        # indiscernable d'une replique jamais prononcee — et « l'agent
        # reste muet » ne disait pas ou la chaine se rompait.
        _log.info("agent (hors LLM) : %s  [%d ech. a %d Hz]",
                  texte, parole.pcm.size, parole.taux)
        # L'envoi du parole.debut vit DANS le try : une annulation frappant
        # cet await peut arriver alors que la trame est deja partie sur le
        # transport — Unreal a le debut, nous avons l'exception. Le laisser
        # hors du bloc recreait exactement le trou que ce finally pretend
        # fermer. Le prix : un parole.fin sans debut si l'annulation frappe
        # AVANT le depart reel — inoffensif cote Unreal, la ou un debut sans
        # fin verrouille le micro.
        try:
            await self._envoyer(ws, "parole.debut", texte=texte, emotion=emotion)
            await self._envoyer_audio(
                ws,
                MorceauAudio(pcm=parole.pcm, taux=parole.taux, texte=texte,
                             premier=True),
                seq=0,
            )
        finally:
            try:
                await self._envoyer(ws, "parole.fin")
            except websockets.ConnectionClosed:
                # Socket morte : Unreal fera son propre menage. On n'avale
                # PAS CancelledError ici : si la PREMIERE annulation de la
                # tache frappe cet await (le corps du try fini), l'avaler
                # faisait survivre la tache a son propre cancel() — asyncio
                # ne la re-delivre pas — et l'appelant continuait de
                # streamer sur une session que le demontage croyait arretee.
                pass

    async def _replique_de_repli(self, ws: ServerConnection, cle: str) -> None:
        """Fait parler l'agent malgre une panne, plutot que de le laisser muet."""
        texte = self._repli.get(cle, self._repli.get("indisponible", ""))
        if not texte:
            return
        try:
            await self._dire(ws, texte)
        except websockets.ConnectionClosed:
            # Unreal est parti : plus personne a qui parler, et surtout pas
            # de second envoi sur une socket morte.
            _log.info("repli impossible : connexion fermee")
        except Exception:
            # Le TTS lui-meme est tombe : on previent, Unreal affichera
            # son propre repli. Une borne muette reste preferable a une
            # borne gelee.
            _log.exception("repli TTS indisponible")
            try:
                await self._envoyer(ws, "erreur", code="tts_indisponible", repli=texte)
            except websockets.ConnectionClosed:
                pass   # la socket est morte aussi : rien de plus a faire

    # -- Tour de parole ---------------------------------------------------

    def _retirer_tache(self, tache: asyncio.Task) -> None:
        """Oublie une tache finie, et journalise ce qu'elle a pu avaler.

        Sans ce rappel, une exception sortant d'une tache que personne
        n'attend ne laissait qu'un « Task exception was never retrieved »
        au fond de la console — une panne invisible dans les logs
        applicatifs.
        """
        self._taches_parole.discard(tache)
        if self._tache_intro is tache:
            self._tache_intro = None
        if self._tache_relance is tache:
            self._tache_relance = None
        if not tache.cancelled() and tache.exception() is not None:
            _log.error("tache de parole tombee : %r", tache.exception())

    def _paroles_en_vie(self) -> int:
        """Tout ce qui tient ou attend le verrou de parole.

        L'intro et la relance comptent : la borne « un qui joue + un qui
        attend » ne comptait d'abord que les tours, et deux segments d'echo
        pouvaient s'empiler derriere l'intro — la replique la plus longue —
        exactement la spirale que la borne devait empecher.
        """
        n = sum(1 for t in self._taches_parole if not t.done())
        for t in (self._tache_intro, self._tache_relance):
            if t is not None and not t.done():
                n += 1
        return n

    def _lancer_tour(self, ws: ServerConnection, brut: bytes) -> None:
        """Traite un enonce en tache, pour que la reception reste libre."""
        # Au plus une parole qui joue et une qui attend — intro et relance
        # comprises. Sans cette borne, chaque enonce s'empilait sur le verrou
        # de parole : un visiteur qui parle deux fois pendant une longue
        # replique (echo du haut-parleur, vrai bavard) recevait des reponses
        # a des enonces vieux de 10-20 s, et le compteur de questions
        # avancait d'autant — spirale de latence garantie sur une borne
        # bruyante.
        if self._paroles_en_vie() >= 2:
            _log.warning("tour deja en attente — enonce ignore")
            return
        tache = asyncio.create_task(self._traiter_audio(ws, brut))
        self._taches_parole.add(tache)
        tache.add_done_callback(self._retirer_tache)

    def _annuler_paroles(self) -> None:
        """Coupe l'intro, la relance ET les tours — le visiteur est parti."""
        for attribut in ("_tache_intro", "_tache_relance"):
            tache = getattr(self, attribut, None)
            if tache is not None and not tache.done():
                tache.cancel()
            setattr(self, attribut, None)

        for tour in list(self._taches_parole):
            if not tour.done():
                tour.cancel()

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
        a_parle = False

        # Sous le verrou de parole : un enonce du visiteur arrivant pendant
        # l'accueil attendra la fin de l'intro au lieu de s'y entrelacer.
        async with self._parole:
            try:
                a_parle = await self._jouer_flux(ws, self.pipeline.intro())

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

    async def _jouer_flux(self, ws: ServerConnection, flux) -> bool:
        """Streame un flux de MorceauAudio puis sa Replique finale.

        Rend vrai si l'agent a effectivement parle. Une Replique portant du
        texte sans qu'aucun audio ne l'ait precedee — le « rien compris » du
        STT — est synthetisee ici : avant, seul l'evenement emotion partait,
        et l'agent demandait de repeter... en silence.
        """
        seq = 0
        a_parle = False
        fin_envoyee = False

        try:
            async for element in flux:
                if isinstance(element, MorceauAudio):
                    if not a_parle:
                        await self._envoyer(ws, "parole.debut", texte=element.texte,
                                            emotion=self._emotion_pressentie())
                        a_parle = True
                    await self._envoyer_audio(ws, element, seq)
                    seq += 1
                else:
                    if a_parle and not fin_envoyee:
                        await self._envoyer(ws, "parole.fin")
                        fin_envoyee = True
                    elif element.texte:
                        # _dire garantit lui-meme debut ET fin, panne
                        # comprise — inutile (et faux) de re-clore ici.
                        await self._dire(ws, element.texte, element.emotion)
                        a_parle = True
                        fin_envoyee = True
                    await self._conclure(ws, element)

        except Exception:
            # Le flux tombe APRES des parole.audio deja emis : sans cloture,
            # Unreal voyait arriver le parole.debut du repli sans jamais la
            # fin de la replique en cours — deux repliques ouvertes a la
            # fois, exactement les deux voix superposees qu'on a chassees.
            # `fin_envoyee` evite l'exces inverse : une panne APRES la
            # cloture (dans _conclure) ne doit pas envoyer un second fin.
            if a_parle and not fin_envoyee:
                try:
                    await self._envoyer(ws, "parole.fin")
                except websockets.ConnectionClosed:
                    pass
            raise

        finally:
            # Ferme le generateur maintenant, pas a son ramassage : tant
            # qu'il vit, le flux SSE vers llama.cpp reste ouvert et le
            # modele continue de generer sur le GPU partage.
            await flux.aclose()

        return a_parle

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

            # La session a pu se fermer pendant l'attente du verrou
            # (presence.perdue arrive entre la mise en file et l'execution).
            # Executer ce tour quand meme ferait parler l'agent hors session
            # — et si un nouveau visiteur venait d'arriver, l'echange
            # parasite s'ecrirait dans SA session : historique pollue,
            # machine a etats avancee avant meme son intro.
            if not self._occupe.locked():
                _log.info("parole ignoree : aucune session en cours")
                return

            mesure = Mesure()

            try:
                await self._jouer_flux(
                    ws, self.pipeline.tour_de_parole(audio, TAUX_VISITEUR, mesure)
                )

            except websockets.ConnectionClosed:
                # Unreal est parti en plein streaming : rien a rejouer, la
                # liberation de session est faite par le finally de
                # _connexion. Surtout ne pas tenter un repli sur cette
                # socket morte.
                _log.info("connexion fermee en plein tour de parole")
                return

            except httpx.HTTPError:
                # C'est le LLM qui ne repond pas — pas le visiteur qui
                # articule mal. « Repetez. » l'accuserait a tort et pour
                # rien : on annonce plutot un poste ferme.
                _log.exception("llama.cpp injoignable pendant le tour")
                await self._replique_de_repli(ws, "indisponible")
                return

            except asyncio.TimeoutError:
                # Un etage a depasse sa borne (STT ou TTS fige) : panne de
                # la pile, pas faute du visiteur. Meme traitement qu'un LLM
                # injoignable.
                _log.exception("etage du pipeline hors delai")
                await self._replique_de_repli(ws, "indisponible")
                return

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

    async def _traiter_silence(self, ws: ServerConnection) -> None:
        """Unreal signale un visiteur muet : l'agent le relance.

        Meme discipline que les tours de parole — verrou, gardes, replis.
        """
        async with self._parole:
            if self.pipeline.etat.terminee:
                return

            try:
                await self._jouer_flux(ws, self.pipeline.relance_silence())

            except websockets.ConnectionClosed:
                _log.info("connexion fermee pendant la relance")
                return

            except Exception:
                # Une relance qui echoue n'a pas de repli dedie : le repli
                # « incompris » accuserait un visiteur qui n'a rien dit. On
                # se tait, l'abandon d'Unreal fera son office.
                _log.exception("echec de la relance")
                return

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

    def _oublier_fermeture(self, tache: asyncio.Task) -> None:
        """Meme discipline que _retirer_tache : rien ne tombe en silence."""
        self._taches_fermeture.discard(tache)
        if not tache.cancelled() and tache.exception() is not None:
            _log.warning("fermeture de l'ancienne connexion : %r",
                         tache.exception())

    def _liberer_session(self) -> None:
        """Coupe les paroles, remet le scenario a zero, relache le verrou."""
        self._annuler_paroles()
        self.pipeline.reinitialiser()
        if self._occupe.locked():
            self._occupe.release()

    async def _connexion(self, ws: ServerConnection) -> None:
        ancienne = self._ws_actif
        self._ws_actif = ws
        if ancienne is not None:
            # Unreal a redemarre : la borne est mono-poste, la derniere
            # connexion gagne. On libere la session de l'ancienne ICI —
            # pas dans son finally, qui peut arriver de longues secondes
            # plus tard (timeout TCP) et aurait sinon detruit la session
            # que la nouvelle connexion vient d'ouvrir.
            #
            # La fermeture part en tache, JAMAIS attendue : un pair a
            # moitie mort (processus Unreal gele, socket ouverte) retient
            # close() pendant tout le close_timeout — 10 s pendant
            # lesquelles le presence.detectee de la nouvelle connexion
            # attendait en file, et le watchdog d'Unreal declarait le
            # sidecar mort. Rien ici ne depend de la fin de cette fermeture.
            _log.warning("nouvelle connexion : l'ancienne est remplacee")
            self._liberer_session()
            fermeture = asyncio.create_task(ancienne.close())
            self._taches_fermeture.add(fermeture)
            fermeture.add_done_callback(self._oublier_fermeture)

        _log.info("Unreal connecte")
        try:
            async for message in ws:
                # Une socket remplacee peut encore livrer des messages
                # tamponnes (ou reprendre la main au milieu d'un await) :
                # plus un seul ne doit toucher la session, qui appartient
                # desormais a la nouvelle connexion.
                if self._ws_actif is not ws:
                    _log.info("message d'une connexion remplacee — ignore")
                    break

                if isinstance(message, bytes):
                    # Hors session, un enonce n'a pas de destinataire : c'est
                    # un segment VAD parti apres presence.perdue (le visiteur
                    # parlait en quittant la zone). Le traiter lancait un
                    # tour fantome complet — STT, LLM, TTS — qui parlait dans
                    # le vide, ou pire, s'ecrivait dans la session du
                    # visiteur suivant.
                    if not self._occupe.locked():
                        _log.info("trame audio hors session — ignoree")
                        continue
                    # En tache, jamais attendu ici : la reception doit rester
                    # capable de lire un presence.perdue pendant le tour.
                    self._lancer_tour(ws, message)
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
            # redemarrage manuel du sidecar. Mais on ne nettoie QUE si cette
            # connexion est encore la connexion active : une socket remplacee
            # qui agonise n'a plus le droit de toucher a la session.
            if self._ws_actif is ws:
                self._ws_actif = None
                self._liberer_session()
                _log.info("connexion fermee — session liberee")
            else:
                _log.info("connexion remplacee fermee — session intacte")

    async def _router(self, ws: ServerConnection, evenement: str) -> None:
        if evenement == "presence.detectee":
            if self._occupe.locked():
                _log.warning("presence signalee alors qu'une session court deja")
                return
            await self._occupe.acquire()
            # La connexion a pu etre remplacee PENDANT l'attente du verrou :
            # ce handler ne represente plus personne, et garder le verrou
            # ici le faisait fuir — plus aucune session ne demarrait ensuite.
            if self._ws_actif is not ws:
                self._occupe.release()
                _log.warning("presence d'une connexion remplacee — ignoree")
                return
            # Ceinture et bretelles : si un tour d'une session precedente
            # traine encore (il sera de toute facon ignore par ses propres
            # gardes), on le coupe avant d'ouvrir la session neuve.
            self._annuler_paroles()
            self.pipeline.reinitialiser()
            await self._envoyer(ws, "session.demarree", avatar=self._avatar)

            # Re-verifie APRES l'envoi : ce send peut suspendre (pause de
            # flow-control) et la connexion etre remplacee pendant qu'il
            # dort — le remplacement a alors deja libere la session, et
            # creer l'intro ici fabriquait une intro fantome liee a une
            # socket morte, qui pouvait ecraser _tache_intro et rendre la
            # vraie intro inannulable. Ne PAS relacher le verrou : il ne
            # nous appartient plus (libere par le remplacement, peut-etre
            # deja repris par la nouvelle session).
            if self._ws_actif is not ws:
                _log.warning("connexion remplacee pendant l'ouverture — intro annulee")
                return
            _log.info("session demarree")

            # L'intro tourne A COTE de la boucle de reception, pas dedans.
            # L'attendre ici bloquerait `async for message in ws` pendant
            # toute la generation — LLM puis TTS, plusieurs secondes — et le
            # sidecar serait sourd au moment ou il parle : ni l'audio du
            # visiteur, ni son depart ne seraient traites.
            self._tache_intro = asyncio.create_task(self._jouer_intro(ws))
            self._tache_intro.add_done_callback(self._retirer_tache)

        elif evenement == "visiteur.silencieux":
            # Unreal a laisse DelaiReponseVisiteur au visiteur apres la fin
            # de la derniere replique : personne n'a parle, l'agent relance.
            if not self._occupe.locked():
                _log.warning("silence signale hors session — ignore")
                return
            if self._tache_relance is not None and not self._tache_relance.done():
                _log.warning("relance deja en cours — ignoree")
                return
            self._tache_relance = asyncio.create_task(self._traiter_silence(ws))
            self._tache_relance.add_done_callback(self._retirer_tache)

        elif evenement in ("presence.perdue", "session.reset"):
            # Le visiteur part : on coupe l'intro ET le tour en cours plutot
            # que de continuer a parler dans le vide. C'est possible parce
            # que les tours tournent en taches — la reception n'attend plus
            # la fin d'une replique pour lire ce message.
            self._liberer_session()
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

        # max_size : ~2 Mo, soit plus d'une minute d'audio 16 kHz PCM16.
        # Aucun segment VAD legitime n'approche cette taille ; au-dela,
        # c'est un client accidentel ou malveillant sur le loopback, et
        # None l'aurait laisse allouer sans limite.
        async with serve(self._connexion, hote, port, max_size=2 * 1024 * 1024):
            _log.info("sidecar a l'ecoute sur ws://%s:%d", hote, port)
            await asyncio.Future()   # tourne jusqu'a interruption
