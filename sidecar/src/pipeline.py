"""Orchestration STT -> LLM -> TTS, en streaming.

Le principe qui fait tout le gain : on ne serialise pas les etages. Des
qu'une phrase sort du LLM, elle part au TTS pendant que le modele ecrit la
suite. Le premier son tombe donc bien avant que la reponse soit complete.

La machine a etats reprend le Narrative Design Convai
(voir docs/NARRATIVE-DESIGN.md) :

    INTRO -> Interrogatoire (5 a 10 questions) -> accepte | refus

Le plancher et le plafond de questions ne dependent pas du bon vouloir du
modele : ils sont imposes par bascule de grammaire GBNF.
"""

from __future__ import annotations

import asyncio
import re
from collections.abc import AsyncIterator, Awaitable, Callable
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import yaml

import logging

from .llm import ClientLLM, Replique
from .machine_etats import Phase, SessionEtat

_log = logging.getLogger("sidecar.pipeline")
from .metrics import Mesure
from .stt import Transcripteur
from .texte import nettoyer_pour_tts
from .tts import Synthetiseur

# Bornes de temps par etage. Un moteur qui se fige (driver GPU, OOM
# ctranslate2) tenait le verrou de parole indefiniment : la borne restait
# muette pour toujours, et chaque incident abandonnait un thread bloque —
# l'executor (~32 threads) finissait par s'epuiser sur des heures de
# fonctionnement. Le depassement vaut panne franche : le serveur repond par
# le repli « indisponible » et la session reprend la main.
# NB : wait_for abandonne l'ATTENTE, pas le thread. Un thread vraiment fige
# reste perdu — c'est accepte, l'alternative etait le gel complet.
DELAI_STT = 20.0   # Whisper medium, pire cas CPU sur un long segment VAD
DELAI_TTS = 10.0   # Piper met ~0,2 s par phrase : 10 s, c'est une panne


@dataclass
class MorceauAudio:
    """Un fragment de parole pret a etre joue.

    L'animation ne transite plus par ici : Audio2Face anime le visage cote
    Unreal, directement depuis la trame audio.
    """

    pcm: np.ndarray
    taux: int
    texte: str
    premier: bool


class Pipeline:
    def __init__(self, config: dict, racine: Path):
        self.config = config
        self.racine = racine

        # La config se valide AVANT de charger le moindre poids : plusieurs
        # gigaoctets et une minute d'attente pour finir sur une faute de
        # frappe seraient une punition inutile.
        #
        # Le gabarit de prompt DEPEND DE LA FAMILLE DU MODELE : ChatML pour
        # Qwen, [INST] pour Mistral et ses derives (NeMo Minitron). Servir
        # du ChatML a un Mistral ne provoque aucune erreur — le modele
        # repond, mais a cote : releve en essai reel, l'agent repetait une
        # phrase sans rapport et jouait le role du visiteur. Un mauvais
        # gabarit ne se voit que dans la qualite des reponses.
        self._gabarit = config["llm"].get("gabarit", "chatml")
        if self._gabarit not in ("chatml", "mistral"):
            raise RuntimeError(
                f"llm.gabarit inconnu : {self._gabarit!r} (chatml | mistral)"
            )

        self.stt = Transcripteur(config["stt"])
        self.llm = ClientLLM(config["llm"], racine)
        self.tts = Synthetiseur(config["tts"], racine)

        chemin = racine / config["scenario"]["fichier"]
        self.scenario = yaml.safe_load(chemin.read_text(encoding="utf-8"))

        # L'intro est-elle recitee ou generee ?
        #
        # RECITEE PAR DEFAUT, et c'est un choix de mise au point. L'intro
        # etait generee sous la grammaire d'entretien — la seule que la
        # machine a etats sache produire a zero question. Or entretien.gbnf
        # impose « (reaction) question? » : un creneau de reaction, alors
        # qu'a l'intro il n'y a rien a quoi reagir, et une terminaison en
        # « ? » obligatoire, alors que le scenario demande un imperatif
        # (« Declinez votre identite. »).
        #
        # Le modele remplissait donc le creneau vide avec la seule chose que
        # la persona lui souffle — le soupcon. Releve trois fois sur trois le
        # 31/07/2026, avant que le visiteur ait dit un mot :
        #
        #   « vous mentez-vous sur votre destination ? »
        #   « vous mentez quand vous dites etre ici pour des affaires... ? »
        #   « vous mentez, denoncez-vous, avouez votre velocite. [...] deten? »
        #
        # La troisieme montre l'autre effet du plafond : « car{25,110} "?" »
        # force le point d'interrogation a 110 caracteres, ou que le modele
        # en soit — d'ou la coupure en plein mot.
        #
        # Reciter supprime le LLM du seul tour de parole que TOUT visiteur
        # entend, et rend l'accueil identique a chaque session : c'est ce
        # qu'on veut d'une borne, et ce qu'il faut pour valider le reste de
        # la chaine sans qu'une variable de plus s'y ajoute. La generation
        # reviendra avec sa propre grammaire, aux finitions.
        self._intro_fixe = config["scenario"].get("intro_fixe", True)

        # Texte de l'accueil nominal. La cle `accueil` de premier niveau est
        # la bonne ; repli.accueil ne sert que de filet, et reste court parce
        # qu'il est le repli d'URGENCE — celui qu'on entend quand quelque
        # chose est deja casse. Les deux ne visent pas la meme situation, et
        # ne doivent donc pas avoir la meme longueur.
        self._texte_accueil = (
            (self.scenario.get("accueil") or "").strip()
            or (self.scenario.get("repli") or {}).get("accueil", "")
        )

        # Verifie au CABLAGE : sans cette phrase, la borne accueillerait le
        # public en silence, et on ne le decouvrirait que devant lui.
        if self._intro_fixe and not self._texte_accueil:
            raise RuntimeError(
                "scenario.intro_fixe est actif mais ni `accueil` ni "
                "repli.accueil ne portent de texte : aucune phrase a reciter"
            )

        interro = self.scenario["interrogatoire"]
        self.etat = SessionEtat(
            questions_min=interro.get("questions_min", 5),
            questions_max=interro.get("questions_max", 10),
        )
        self.historique: list[tuple[str, str]] = []

        # Emotion de la DERNIERE replique close. Le tag [EMOTION:] arrive
        # apres le texte parle — c'est voulu, la lecture demarre sans
        # l'attendre — donc la replique en cours ne connait jamais la
        # sienne au moment ou on la synthetise. On prosodie sur la
        # precedente : un garde qui vient de se facher garde le ton dans
        # la phrase suivante, ce qui est plus juste qu'une remise a plat
        # a chaque tour.
        self.emotion_courante = "Stare"

    def reinitialiser(self) -> None:
        """Remet la session a zero — appele quand un visiteur quitte la zone."""
        self.etat.reinitialiser()
        self.historique.clear()

    # -- Construction du prompt -----------------------------------------

    def _systeme(self) -> str:
        """Bloc systeme — STRICTEMENT statique, et SANS description du format.

        Deux regles apprises a la mesure :

        1. Rien de variable ici. llama.cpp met en cache le prefixe commun
           (`cache_prompt`) ; la moindre variation par tour ferait tout
           recalculer. Le compteur de questions vit donc cote utilisateur.
           (Gain constate en le sortant d'ici : 1064 -> 611 ms.)

        2. **Aucune mention des tags.** Une version anterieure detaillait
           ici "[EMOTION:X][VERDICT:Y]" et la liste des emotions. Resultat :
           le modele recrachait ce vocabulaire en prose —
           "Neutral Happy Concerned Angry X Verditt :Neutral..." — puisque
           la grammaire lui interdit d'ecrire les vrais crochets.
           Avec du decodage contraint, decrire le format n'est pas
           redondant : c'est nuisible. La grammaire suffit.
        """
        s = self.scenario
        return "\n\n".join([
            s["persona"].strip(),
            s["langue"].strip(),
            s["intro"].strip(),
            s["interrogatoire"]["objectif"].strip(),
        ])

    def _etat_courant(self) -> str:
        """Rappel de l'avancement — cote utilisateur, car il change a chaque tour."""
        s = self.scenario
        e = self.etat

        if e.phase is Phase.INTRO:
            # Le VOUVOIEMENT se rappelle ICI AUSSI. Il ne vivait que dans le
            # rappel d'interrogatoire ci-dessous : l'intro, qui ne le voit
            # jamais, tutoyait le visiteur des la premiere phrase — « qui
            # es-tu ? » releve en essai reel le 30/07/2026. Or c'est
            # precisement la replique que tout le monde entend.
            return ("[Debut du controle. Interpelle le visiteur. "
                    "VOUVOIE-le : « vous », jamais « tu ».]")

        # Le compteur ne sort PLUS du code. Il gouvernait le modele, qui
        # produisait alors « la question numero quatre » au lieu de mener un
        # entretien : rien dans « [3 question(s) posee(s), encore 2] »
        # n'invite a reagir a ce qui vient d'etre dit.
        #
        # On lui donne desormais ce qu'un garde a reellement en tete : ce
        # qu'il doit etablir. Le compteur reste, mais uniquement pour la
        # bascule de grammaire — invisible du modele.
        sujets = "\n".join(
            f"  - {sujet}" for sujet in s["interrogatoire"]["sujets"]
        )
        # REAGIR n'est pas REPETER. La persona interdit de repeter la reponse
        # du visiteur ; elle exige au contraire de relever ce qui est flou ou
        # contradictoire (« ce qui est evasif doit etre repris. C'est ton
        # metier »). J'avais retire toute consigne de reaction en les
        # confondant : l'agent enchainait alors ses questions comme un
        # formulaire, aucune reponse n'avait de consequence audible, et le
        # dialogue ne ressemblait a rien d'humain. La grammaire reserve
        # desormais une phrase breve a cette reaction, avant la question.
        #
        # Les questions deja posees, litteralement : « tu ne reposes jamais
        # une question deja posee » ne suffit pas a un 3B, il faut le lui
        # montrer — releve en essai reel, cinq fois « pourquoi vis-tu ici »
        # d'affilee. Mais les QUATRE dernieres seulement : a huit, la liste
        # d'interdits dominait le prompt et poussait le modele vers la seule
        # sortie sure — des questions d'un mot, inedites par construction.
        deja = [agent for _, agent in self.historique if agent]
        interdites = ""
        if deja:
            liste = "\n".join(f"  - {q}" for q in deja[-4:])
            interdites = (
                "\n[Tu as DEJA dit ceci. Ne le repete pas, ne le reformule "
                f"pas, passe a autre chose :\n{liste}]"
            )

        # LE TIC D'OUVERTURE, et pourquoi la liste ci-dessus ne l'attrape pas.
        #
        # Le modele contourne l'interdiction de repetition en variant la FIN
        # de sa phrase et en reprenant son DEBUT : aucune replique n'est
        # identique a une autre, la consigne est donc formellement respectee,
        # et l'entretien sonne pourtant comme un disque raye. Mesure du
        # 31/07/2026 par bench/dialogue_test.py, profil menacant :
        # « Doutez-vous vraiment que » ouvrait 5 repliques sur 7 (71 %).
        #
        # On lui montre donc ses ouvertures, litteralement — meme lecon que
        # pour les questions deja posees : a un 3B, une regle abstraite
        # (« jamais deux fois la meme tournure ») ne vaut pas une liste
        # concrete de ce qu'il vient de faire.
        #
        # Six, et non quatre comme au-dessus : trois mots chacune ne pesent
        # presque rien dans le prompt, et le tic s'installe sur la duree.
        ouvertures: list[str] = []
        for replique in deja[-6:]:
            mots = replique.split()
            if len(mots) < 2:
                continue
            tete = " ".join(mots[:3]).rstrip(",.;:!?»«")
            if tete and tete.lower() not in (o.lower() for o in ouvertures):
                ouvertures.append(tete)
        if ouvertures:
            liste_ouvertures = ", ".join(f"« {o} »" for o in ouvertures)
            interdites += (
                "\n[Tu as deja COMMENCE des repliques par : "
                f"{liste_ouvertures}. Commence celle-ci par de tout autres "
                "mots.]"
            )

        rappel = (
            "[Ce sur quoi porte le controle :\n"
            f"{sujets}\n"
            "Commence par UNE phrase tres breve qui reagit a ce que le "
            "visiteur vient de dire — doute, reproche, incoherence relevee — "
            "sans le repeter mot pour mot. Puis pose UNE question complete, "
            "avec sujet et verbe, sur un point encore obscur.\n"
            "Si sa reponse est du charabia ou hors sujet, fais-le lui "
            "remarquer sechement et repose ta question autrement.\n"
            "VOUVOIE le visiteur : « vous », jamais « tu ».]"
            f"{interdites}"
        )

        if e.nb_questions >= e.questions_max:
            # LE RAPPEL D'INTERROGATOIRE EST RETIRE ICI, et c'est tout
            # l'objet de ce bloc.
            #
            # Il etait joint au verdict jusqu'au 31/07/2026. Or il ordonne
            # de commencer par « doute, reproche, incoherence relevee » :
            # le modele etait amorce a la suspicion a l'instant precis ou il
            # devait juger. S'y ajoutait l'historique, plein des propres
            # remarques de l'agent — « Incoherence relevee », « vous
            # mentez » — qui se lisent comme des preuves contre le visiteur,
            # quoi que celui-ci ait reellement repondu.
            #
            # Resultat mesure par bench/dialogue_test.py : REFUSE sur les
            # TROIS profils, y compris le visiteur cooperatif, coherent et
            # sans danger. L'agent se refusait sur son propre ton.
            #
            # Ce n'etait pas un defaut de dialogue mais une BRANCHE MORTE du
            # scenario : personne n'etant jamais accepte, le tampon
            # « accepte » et toute sa scenographie ne jouaient jamais. La
            # borne n'avait qu'une seule fin.
            #
            # On donne donc des criteres FACTUELS, portant sur ce que le
            # visiteur a dit — verifiables, la ou « coherent et sans
            # danger » demandait au modele de juger une impression.
            return (
                "[Le controle est termine. Tu ne poses PLUS de question.\n"
                "Rends ton verdict MAINTENANT, dans une replique separee.\n"
                "Juge sur CE QUE LE VISITEUR A REPONDU, et sur rien d'autre. "
                "Le doute que tu as exprime pendant l'entretien etait ton "
                "metier : ce n'est pas une preuve contre lui.\n"
                "S'il a decline son identite, donne un motif de visite "
                "plausible et ne s'est pas contredit : "
                f"{s['verdicts']['accepte']['objectif'].strip()}\n"
                "S'il a refuse de repondre, s'est contredit, ou s'est montre "
                "menacant : "
                f"{s['verdicts']['refus']['objectif'].strip()}\n"
                "VOUVOIE-le : « vous », jamais « tu ».]"
            )

        return rappel

    def _construire_prompt(self, entree_visiteur: str) -> str:
        """Assemble le prompt au gabarit de la famille du modele."""
        dernier = f"{entree_visiteur}\n{self._etat_courant()}"
        if self._gabarit == "mistral":
            return self._prompt_mistral(dernier)
        return self._prompt_chatml(dernier)

    def _prompt_chatml(self, dernier_tour: str) -> str:
        """Format ChatML — Qwen."""
        parties = [
            "<|im_start|>system\n", self._systeme(), "<|im_end|>\n",
        ]
        for visiteur, agent in self.historique:
            parties.append(f"<|im_start|>user\n{visiteur}<|im_end|>\n")
            parties.append(f"<|im_start|>assistant\n{agent}<|im_end|>\n")
        parties.append(f"<|im_start|>user\n{dernier_tour}<|im_end|>\n")
        parties.append("<|im_start|>assistant\n")
        return "".join(parties)

    def _prompt_mistral(self, dernier_tour: str) -> str:
        """Format [INST] — Mistral, NeMo Minitron et derives.

        Mistral n'a pas de role systeme : la consigne se loge dans le
        premier bloc [INST]. On lui en donne un A LUI, clos par un accuse
        de reception, plutot que de la coller au premier tour du visiteur.
        C'est ce qui garde le prefixe RIGOUREUSEMENT statique — condition
        du cache de prompt de llama.cpp, qui vaut 450 ms par tour (voir
        _systeme). Colle au premier tour, le prefixe changerait des que
        l'historique bouge, et tout serait recalcule a chaque fois.
        """
        parties = [f"<s>[INST] {self._systeme()} [/INST] Compris.</s>"]
        for visiteur, agent in self.historique:
            parties.append(f"[INST] {visiteur} [/INST] {agent}</s>")
        parties.append(f"[INST] {dernier_tour} [/INST]")
        return "".join(parties)

    # -- Tour de parole ---------------------------------------------------

    async def tour_de_parole(
        self,
        audio_visiteur: np.ndarray,
        taux: int = 16000,
        mesure: Mesure | None = None,
        au_transcrit: Callable[[str, float], Awaitable[None]] | None = None,
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """Traite un tour complet : audio entrant -> audio sortant.

        Emet des MorceauAudio au fil de l'eau, puis une Replique finale
        portant l'emotion et le verdict.

        `au_transcrit` est appele des que le STT a rendu son texte, avant
        toute generation — meme motif que `au_premier_jeton` cote LLM. Il
        sert a faire REMONTER la transcription : le sidecar la journalisait
        depuis toujours, mais ne la disait a personne. Or c'est la seule
        facon de distinguer un agent qui repond mal d'un agent qui a mal
        entendu, et cette distinction decide de l'endroit ou l'on cherche.
        """
        m = mesure or Mesure()
        m.marquer("debut")

        # --- 1. Transcription -------------------------------------------
        # to_thread : faster-whisper est synchrone et prend de quelques
        # centaines de ms a plusieurs secondes. L'appeler directement gelait
        # TOUT l'event loop — ping/pong websockets compris, et surtout le
        # presence.perdue d'un visiteur qui part pendant la transcription.
        texte_visiteur = await asyncio.wait_for(
            asyncio.to_thread(self.stt.transcrire, audio_visiteur, taux),
            timeout=DELAI_STT,
        )
        m.marquer("stt_fin")

        # Journalise, parce que sans cela on ne peut pas distinguer un LLM qui
        # repond mal d'un LLM qui recoit du charabia. C'est le meme angle mort
        # qui a masque, tour a tour, une liaison serie muette, des courbes
        # faciales ecrites dans le vide et un flux audio fragmente.
        duree_audio = audio_visiteur.size / max(taux, 1)
        _log.info("visiteur (%.2f s) : %s", duree_audio,
                  texte_visiteur if texte_visiteur else "(rien compris)")

        # ATTENDU, et non lance en tache de fond : l'ordre compte. La
        # transcription doit partir AVANT le parole.debut du tour qu'elle
        # declenche, sinon le journal donne la reponse avant la question et
        # devient penible a relire. Le cout est nul — un envoi sur une
        # socket locale, avant plusieurs centaines de ms de generation.
        if au_transcrit is not None:
            await au_transcrit(texte_visiteur, duree_audio)

        if not texte_visiteur:
            yield Replique(
                texte=self.scenario["repli"]["incompris"],
                emotion="Concerned",
                verdict="EN_COURS",
            )
            return

        async for element in self._generer(texte_visiteur, m):
            yield element

    async def intro(
        self,
        mesure: Mesure | None = None,
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """L'agent interpelle le visiteur des son arrivee.

        Le seul tour de parole qui ne repond a rien. Tout etait deja prevu
        pour lui — la phase INTRO de la machine a etats, la section `intro`
        du scenario, la replique de repli `accueil` — mais rien ne l'appelait :
        une borne ou l'agent attendait que le visiteur parle le premier, ce
        qu'un visiteur ne fait jamais devant un garde-frontiere muet.

        Dans l'ancien projet, ce role revenait au Narrative Design de Convai.

        Aucune transcription ici : l'etat de la machine suffit a dire au
        modele ce qu'on attend de lui, via _etat_courant().
        """
        m = mesure or Mesure()
        m.marquer("debut")
        m.marquer("stt_fin")   # pas de transcription : l'etape est nulle

        if self._intro_fixe:
            async for element in self._reciter(
                self._texte_accueil, m, emotion="Stare"
            ):
                yield element
            return

        async for element in self._generer("", m):
            yield element

    async def _reciter(
        self,
        texte: str,
        m: Mesure,
        emotion: str = "Stare",
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """Dit une phrase ecrite d'avance, sans passer par le modele.

        Meme contrat de sortie que _generer — des MorceauAudio puis une
        Replique finale — pour que serveur.py n'ait pas a distinguer les
        deux chemins : meme `parole.debut`, meme `parole.fin`, meme entree
        dans l'historique.

        Les marques LLM restent absentes du releve, volontairement. Mesure
        .depuis() rend None sur une marque manquante : latence.jsonl
        montrera un tour sans etage de generation, ce qui est exactement ce
        qui s'est passe. Une marque a zero aurait menti.

        DECOUPE EN PHRASES, comme _generer. Une premiere version synthetisait
        tout d'un bloc — c'etait plus simple, et Unreal a jete la trame :

          Audio rejete : 170496 octets recus hors replique
          (bRepliqueEnCours=0, phase=Accueil)

        L'agent n'a pas prononce son accueil. Le chemin de streaming, lui,
        n'a jamais rate une replique : il envoie ~70 Ko par phrase la ou ce
        bloc unique en faisait 170 Ko (3,9 s d'un coup). Plutot que de
        traiter ce cas a part, on emprunte exactement le meme chemin —
        memes tailles de trames, meme cadence. Un chemin eprouve vaut mieux
        qu'un chemin special, et le premier son part plus tot par-dessus le
        marche.
        """
        prononcable = nettoyer_pour_tts(texte)

        # Decoupe sur la ponctuation finale, en la conservant. Le texte est
        # ecrit a la main dans le scenario : pas de decimales ni
        # d'abreviations a menager ici, contrairement au flux du modele.
        morceaux = [p for p in re.split(r"(?<=[.!?])\s+", prononcable) if p.strip()]

        premier = True
        for phrase in morceaux:
            parole = await asyncio.wait_for(
                asyncio.to_thread(self.tts.synthetiser, phrase, emotion),
                timeout=DELAI_TTS,
            )

            if premier:
                m.marquer("tts_premier_chunk")
                m.marquer("audio_premier_chunk")

            yield MorceauAudio(
                pcm=parole.pcm,
                taux=parole.taux,
                texte=phrase,
                premier=premier,
            )
            premier = False

        m.marquer("fin")

        # L'intro n'a pas de tour visiteur : meme tour fictif que dans
        # _generer, pour que le prompt des tours suivants ne voie pas un
        # trou la ou l'agent a parle.
        self.historique.append(
            ("[Le visiteur se presente au poste.]", prononcable)
        )
        self.etat.avancer("EN_COURS")
        self.emotion_courante = emotion

        _log.info("agent [%s/EN_COURS, recite] : %s", emotion, prononcable)

        yield Replique(texte=prononcable, emotion=emotion, verdict="EN_COURS")

    async def relance_silence(
        self,
        mesure: Mesure | None = None,
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """Le visiteur ne repond pas : l'agent le somme de parler.

        Declenche par Unreal (DelaiReponseVisiteur ecoule apres la fin d'une
        replique). Le silence entre dans l'historique comme un tour visiteur
        a part entiere : un garde qui attend une reponse et ne l'obtient pas
        le fait peser sur la suite de l'entretien — et le tour compte dans
        les questions, comme n'importe quel autre.
        """
        m = mesure or Mesure()
        m.marquer("debut")
        m.marquer("stt_fin")   # pas de transcription : l'etape est nulle

        async for element in self._generer(
            "[Le visiteur ne repond pas. Somme-le de repondre.]", m
        ):
            yield element

    async def _generer(
        self,
        texte_visiteur: str,
        m: Mesure,
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """Generation et synthese entrelacees, communes a l'intro et aux tours.

        Une entree vide est legitime : c'est l'intro. Le prompt ne portera
        alors que le rappel d'etat, qui suffit a amorcer.
        """
        prompt = self._construire_prompt(texte_visiteur)
        grammaire = self.etat.grammaire
        premier = True
        finale: Replique | None = None
        prononcees: list[str] = []

        flux_llm = self.llm.phrases(
            prompt, grammaire,
            au_premier_jeton=lambda: m.marquer("llm_premier_token"),
        )
        try:
            async for phrase, replique in flux_llm:
                if replique is not None:
                    finale = replique
                    break

                if premier:
                    m.marquer("llm_premiere_phrase")

                # Filtre deterministe : un 3B produit de l'ecriture inclusive et
                # du markdown malgre la consigne, et le TTS les prononce.
                prononcable = nettoyer_pour_tts(phrase)
                if not prononcable:
                    continue

                # Meme regle que pour le STT : Piper est synchrone, on le sort
                # de l'event loop pour que le sidecar reste joignable pendant
                # qu'il parle.
                parole = await asyncio.wait_for(
                    asyncio.to_thread(
                        self.tts.synthetiser, prononcable, self.emotion_courante),
                    timeout=DELAI_TTS,
                )

                if premier:
                    m.marquer("tts_premier_chunk")
                    m.marquer("audio_premier_chunk")

                yield MorceauAudio(
                    pcm=parole.pcm,
                    taux=parole.taux,
                    texte=prononcable,
                    premier=premier,
                )
                premier = False
                prononcees.append(prononcable)

        except Exception:
            # Le LLM tombe EN COURS de generation, apres que des phrases ont
            # deja ete jouees. Ce que l'agent a dit est dit : sans cette
            # inscription, le prompt du tour suivant l'ignorait — l'agent
            # reposait sa question, et le compteur divergeait de ce que le
            # visiteur avait reellement vecu.
            if prononcees:
                self.historique.append((
                    texte_visiteur or "[Le visiteur se presente au poste.]",
                    " ".join(prononcees),
                ))
                self.etat.avancer("EN_COURS")
                _log.warning(
                    "generation interrompue apres %d phrase(s) — historique conserve",
                    len(prononcees),
                )
            raise

        finally:
            # Fermer le flux TOUT DE SUITE, pas au ramassage du generateur :
            # tant qu'il vit, la requete SSE reste ouverte et llama.cpp
            # continue de generer — sur le meme GPU qu'Unreal et que l'intro
            # du visiteur suivant. C'est le cas de l'annulation en pleine
            # replique (presence.perdue pendant un ws.send).
            await flux_llm.aclose()

        m.marquer("fin")

        if finale is None:
            return

        # --- 4. Avancement de la machine a etats -------------------------
        # Sur ACCEPTE/REFUSE, la suite — tampon puis invitation a liberer la
        # zone — est geree cote Unreal, comme elle l'a toujours ete.
        # L'intro n'a pas de tour visiteur. On en inscrit un fictif plutot
        # qu'une chaine vide : l'historique sert a reconstruire le prompt des
        # tours suivants, et un tour utilisateur vide y ferait un trou que le
        # modele interpreterait comme un silence du visiteur.
        # NETTOYE avant d'entrer dans l'historique, pas seulement avant le
        # TTS. L'historique reconstruit le prompt des tours suivants : y
        # laisser le texte brut donnait au modele ses propres « madame » en
        # exemple, et il s'imitait de tour en tour. Filtrer la voix sans
        # filtrer la memoire ne corrige que ce qu'on entend.
        self.historique.append(
            (texte_visiteur or "[Le visiteur se presente au poste.]",
             nettoyer_pour_tts(finale.texte))
        )
        self.etat.avancer(finale.verdict)
        self.emotion_courante = finale.emotion   # prosodie du tour suivant

        _log.info("agent [%s/%s, %d question(s)] : %s",
                  finale.emotion, finale.verdict, self.etat.nb_questions, finale.texte)

        yield finale

    async def prechauffer(self) -> float | None:
        """Paye le cout du prefixe systeme avant l'arrivee du visiteur.

        Le bloc systeme est strictement statique (voir _systeme) : c'est lui
        que llama.cpp garde en cache, et il est le meme a tous les tours.
        Peu importe donc le rappel d'etat qu'on lui accole ici.

        Avec intro_fixe, le premier appel au modele n'est plus l'accueil
        mais la premiere question de l'interrogatoire — le prefixe mis en
        cache reste le bon, et c'est desormais ce tour-la qu'on protege du
        cout a froid (31 s mesures sans prechauffage, 955 ms avec).
        """
        return await self.llm.prechauffer(self._construire_prompt(""))

    async def fermer(self) -> None:
        await self.llm.fermer()
