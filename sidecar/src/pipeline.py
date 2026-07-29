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
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
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


@dataclass
class MorceauAudio:
    """Un fragment de parole pret a etre joue et anime."""

    pcm: np.ndarray
    taux: int
    texte: str
    premier: bool
    # Suite de poses MHF_* (nom, debut_s, fin_s), pour le lipsync de repli
    # quand NeuroSync est indisponible. Vide sinon.
    visemes: list[tuple[str, float, float]] = field(default_factory=list)


class Pipeline:
    def __init__(self, config: dict, racine: Path):
        self.config = config
        self.racine = racine

        self.stt = Transcripteur(config["stt"])
        self.llm = ClientLLM(config["llm"], racine)
        self.tts = Synthetiseur(config["tts"], racine)

        chemin = racine / config["scenario"]["fichier"]
        self.scenario = yaml.safe_load(chemin.read_text(encoding="utf-8"))

        interro = self.scenario["interrogatoire"]
        self.etat = SessionEtat(
            questions_min=interro.get("questions_min", 5),
            questions_max=interro.get("questions_max", 10),
        )
        self.historique: list[tuple[str, str]] = []

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
            return "[Debut du controle. Interpelle le visiteur.]"

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
            return (
                f"{rappel}\n"
                "[Le controle est termine. Rends ton verdict MAINTENANT, dans "
                "une replique separee.\n"
                f"Si le visiteur est coherent et sans danger : "
                f"{s['verdicts']['accepte']['objectif'].strip()}\n"
                f"S'il est incoherent, suspect ou menacant : "
                f"{s['verdicts']['refus']['objectif'].strip()}]"
            )

        return rappel

    def _construire_prompt(self, entree_visiteur: str) -> str:
        """Assemble le prompt au format ChatML (Qwen)."""
        parties = [
            "<|im_start|>system\n", self._systeme(), "<|im_end|>\n",
        ]
        for visiteur, agent in self.historique:
            parties.append(f"<|im_start|>user\n{visiteur}<|im_end|>\n")
            parties.append(f"<|im_start|>assistant\n{agent}<|im_end|>\n")
        parties.append(
            f"<|im_start|>user\n{entree_visiteur}\n{self._etat_courant()}<|im_end|>\n"
        )
        parties.append("<|im_start|>assistant\n")
        return "".join(parties)

    # -- Tour de parole ---------------------------------------------------

    async def tour_de_parole(
        self,
        audio_visiteur: np.ndarray,
        taux: int = 16000,
        mesure: Mesure | None = None,
    ) -> AsyncIterator[MorceauAudio | Replique]:
        """Traite un tour complet : audio entrant -> audio sortant.

        Emet des MorceauAudio au fil de l'eau, puis une Replique finale
        portant l'emotion et le verdict.
        """
        m = mesure or Mesure()
        m.marquer("debut")

        # --- 1. Transcription -------------------------------------------
        # to_thread : faster-whisper est synchrone et prend de quelques
        # centaines de ms a plusieurs secondes. L'appeler directement gelait
        # TOUT l'event loop — ping/pong websockets compris, et surtout le
        # presence.perdue d'un visiteur qui part pendant la transcription.
        texte_visiteur = await asyncio.to_thread(
            self.stt.transcrire, audio_visiteur, taux
        )
        m.marquer("stt_fin")

        # Journalise, parce que sans cela on ne peut pas distinguer un LLM qui
        # repond mal d'un LLM qui recoit du charabia. C'est le meme angle mort
        # qui a masque, tour a tour, une liaison serie muette, des courbes
        # faciales ecrites dans le vide et un flux audio fragmente.
        _log.info("visiteur (%.2f s) : %s",
                  audio_visiteur.size / max(taux, 1),
                  texte_visiteur if texte_visiteur else "(rien compris)")

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

        async for element in self._generer("", m):
            yield element

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

        try:
            async for phrase, replique in self.llm.phrases(prompt, grammaire):
                if replique is not None:
                    finale = replique
                    break

                if premier:
                    m.marquer("llm_premier_token")
                    m.marquer("llm_premiere_phrase")

                # Filtre deterministe : un 3B produit de l'ecriture inclusive et
                # du markdown malgre la consigne, et le TTS les prononce.
                prononcable = nettoyer_pour_tts(phrase)
                if not prononcable:
                    continue

                # Meme regle que pour le STT : Piper est synchrone, on le sort
                # de l'event loop pour que le sidecar reste joignable pendant
                # qu'il parle.
                parole = await asyncio.to_thread(self.tts.synthetiser, prononcable)

                if premier:
                    m.marquer("tts_premier_chunk")
                    m.marquer("audio_premier_chunk")

                yield MorceauAudio(
                    pcm=parole.pcm,
                    taux=parole.taux,
                    texte=prononcable,
                    premier=premier,
                    visemes=parole.visemes,
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
        self.historique.append(
            (texte_visiteur or "[Le visiteur se presente au poste.]", finale.texte)
        )
        self.etat.avancer(finale.verdict)

        _log.info("agent [%s/%s, %d question(s)] : %s",
                  finale.emotion, finale.verdict, self.etat.nb_questions, finale.texte)

        yield finale

    async def fermer(self) -> None:
        await self.llm.fermer()
