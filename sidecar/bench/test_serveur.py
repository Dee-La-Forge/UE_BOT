"""Tests du serveur WebSocket — cycle de session, verrous, connexions.

C'est la que la borne casse reellement : trame arrivee hors session,
deconnexion brutale, double connexion apres un redemarrage d'Unreal,
enonces qui s'empilent pendant une replique. Aucun de ces chemins n'etait
teste — le tour fantome (audio traite hors session, ecrit dans la session
du visiteur suivant) aurait ete attrape par le premier de ces cas.

Aucun modele requis : le pipeline est factice, et faster-whisper / piper
sont stubbes avant l'import.

    py -3.12 -m bench.test_serveur
"""

from __future__ import annotations

import asyncio
import json
import sys
import types
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

# Stubs des dependances lourdes : ces tests ne les touchent jamais, mais la
# chaine d'import serveur -> pipeline -> stt/tts les charge au passage.
sys.modules.setdefault(
    "faster_whisper", types.SimpleNamespace(WhisperModel=object))
sys.modules.setdefault(
    "piper", types.SimpleNamespace(PiperVoice=object, SynthesisConfig=object))

import numpy as np  # noqa: E402

import src.serveur as module_serveur  # noqa: E402
from src.llm import Replique  # noqa: E402
from src.machine_etats import SessionEtat  # noqa: E402
from src.pipeline import MorceauAudio  # noqa: E402


# -- Doublures --------------------------------------------------------------

class FauxTTS:
    @staticmethod
    def en_pcm16(audio) -> bytes:
        return b"\x00\x00" * max(len(audio), 1)


class FauxPipeline:
    """Meme surface que Pipeline, sans un seul modele."""

    def __init__(self, config: dict, racine: Path):
        self.scenario = {
            "metahuman": "BP_Test",
            "repli": {
                "accueil": "Papiers.",
                "indisponible": "Poste ferme.",
                "incompris": "Repetez.",
            },
        }
        self.etat = SessionEtat(questions_min=2, questions_max=4)
        self.tts = FauxTTS()
        self.llm = types.SimpleNamespace(url="http://faux")
        self.tours = 0        # tours de parole reellement EXECUTES
        self.intros = 0
        self.resets = 0
        self.delai_tour = 0.0  # simule un tour long (verrou tenu)

    def reinitialiser(self) -> None:
        self.resets += 1
        self.etat.reinitialiser()

    async def _flux(self, texte: str):
        yield MorceauAudio(pcm=np.zeros(8, dtype=np.float32), taux=22050,
                           texte=texte, premier=True)
        yield Replique(texte=texte, emotion="Neutral", verdict="EN_COURS")

    async def tour_de_parole(self, audio, taux=16000, mesure=None):
        self.tours += 1
        if self.delai_tour:
            await asyncio.sleep(self.delai_tour)
        async for element in self._flux("reponse"):
            yield element

    async def intro(self, mesure=None):
        self.intros += 1
        async for element in self._flux("intro"):
            yield element

    async def relance_silence(self, mesure=None):
        async for element in self._flux("relance"):
            yield element

    async def fermer(self) -> None:
        pass


_FIN = object()   # sentinelle : la connexion se ferme


class FauxWS:
    """Socket factice : file de messages entrants, journal des sortants."""

    def __init__(self):
        self._file: asyncio.Queue = asyncio.Queue()
        self.envoyes: list = []
        self.fermee = False

    def pousser(self, message) -> None:
        self._file.put_nowait(message)

    def __aiter__(self):
        return self

    async def __anext__(self):
        message = await self._file.get()
        if message is _FIN:
            raise StopAsyncIteration
        return message

    async def send(self, donnees) -> None:
        self.envoyes.append(donnees)

    async def close(self, *args, **kwargs) -> None:
        self.fermee = True
        self.pousser(_FIN)

    def evenements(self) -> list[str]:
        """Noms des evenements JSON emis, dans l'ordre."""
        noms = []
        for e in self.envoyes:
            if isinstance(e, str):
                noms.append(json.loads(e).get("evenement", "?"))
        return noms


def fabriquer_serveur() -> module_serveur.Serveur:
    config = {
        "serveur": {"hote": "127.0.0.1", "port": 0},
        "llm": {"modele": "faux"},
        "mesures": {"actif": False, "fichier_sortie": "logs/test.jsonl"},
    }
    vrai = module_serveur.Pipeline
    module_serveur.Pipeline = FauxPipeline
    try:
        return module_serveur.Serveur(config, RACINE)
    finally:
        module_serveur.Pipeline = vrai


TRAME = b"\x01\x00" * 1600   # 100 ms d'audio 16 kHz factice


async def _souffler(duree: float = 0.05) -> None:
    """Laisse les taches en file s'executer."""
    await asyncio.sleep(duree)


# -- Cas --------------------------------------------------------------------

async def cas_trame_hors_session() -> list[str]:
    """Une trame recue sans session ne doit lancer AUCUN tour."""
    s = fabriquer_serveur()
    ws = FauxWS()
    tache = asyncio.create_task(s._connexion(ws))

    ws.pousser(TRAME)
    await _souffler()
    ws.pousser(_FIN)
    await tache

    erreurs = []
    if s.pipeline.tours != 0:
        erreurs.append(f"{s.pipeline.tours} tour(s) executes sur une trame hors session")
    if any(e.startswith("parole") for e in ws.evenements()):
        erreurs.append("le serveur a parle sans session")
    return erreurs


async def cas_session_nominale() -> list[str]:
    """presence.detectee -> intro ; trame -> tour ; les evenements sortent."""
    s = fabriquer_serveur()
    ws = FauxWS()
    tache = asyncio.create_task(s._connexion(ws))

    ws.pousser(json.dumps({"evenement": "presence.detectee"}))
    await _souffler()
    ws.pousser(TRAME)
    await _souffler()
    ws.pousser(_FIN)
    await tache

    evts = ws.evenements()
    erreurs = []
    if s.pipeline.intros != 1:
        erreurs.append(f"{s.pipeline.intros} intro(s) au lieu de 1")
    if s.pipeline.tours != 1:
        erreurs.append(f"{s.pipeline.tours} tour(s) au lieu de 1")
    if evts[:1] != ["session.demarree"]:
        erreurs.append(f"premiere emission {evts[:1]} au lieu de session.demarree")
    for attendu in ("parole.debut", "parole.audio", "parole.fin", "emotion"):
        if attendu not in evts:
            erreurs.append(f"evenement {attendu} jamais emis")
    if not erreurs and evts.index("parole.debut") > evts.index("parole.audio"):
        erreurs.append("parole.audio emis avant parole.debut")
    return erreurs


async def cas_file_bornee() -> list[str]:
    """Trois enonces pendant un tour long : le troisieme est jete."""
    s = fabriquer_serveur()
    s.pipeline.delai_tour = 0.2
    ws = FauxWS()
    tache = asyncio.create_task(s._connexion(ws))

    ws.pousser(json.dumps({"evenement": "presence.detectee"}))
    await _souffler()
    ws.pousser(TRAME)
    ws.pousser(TRAME)
    ws.pousser(TRAME)
    await _souffler(0.8)   # laisse les tours retenus se derouler
    ws.pousser(_FIN)
    await tache

    erreurs = []
    if s.pipeline.tours != 2:
        erreurs.append(
            f"{s.pipeline.tours} tour(s) executes au lieu de 2 (1 en cours + 1 en attente)")
    return erreurs


async def cas_tour_fantome() -> list[str]:
    """Trame puis presence.perdue coup sur coup : le tour ne s'execute pas."""
    s = fabriquer_serveur()
    s.pipeline.delai_tour = 0.2
    ws = FauxWS()
    tache = asyncio.create_task(s._connexion(ws))

    ws.pousser(json.dumps({"evenement": "presence.detectee"}))
    await _souffler()
    tours_apres_intro = s.pipeline.tours

    # Le visiteur parle en quittant la zone : la trame arrive, puis le
    # presence.perdue — avant que le tour n'ait pu s'executer.
    ws.pousser(TRAME)
    ws.pousser(json.dumps({"evenement": "presence.perdue"}))
    await _souffler(0.4)

    erreurs = []
    if s.pipeline.tours != tours_apres_intro:
        erreurs.append("le tour fantome s'est execute apres presence.perdue")
    if s._occupe.locked():
        erreurs.append("le verrou de session est reste tenu apres presence.perdue")

    # Et une trame arrivant APRES la fermeture est ignoree des la reception.
    ws.pousser(TRAME)
    await _souffler()
    if s.pipeline.tours != tours_apres_intro:
        erreurs.append("une trame post-session a lance un tour")

    ws.pousser(_FIN)
    await tache
    return erreurs


async def cas_double_connexion() -> list[str]:
    """La socket agonisante d'un PIE relance ne tue pas la session neuve."""
    s = fabriquer_serveur()

    ws1 = FauxWS()
    tache1 = asyncio.create_task(s._connexion(ws1))
    ws1.pousser(json.dumps({"evenement": "presence.detectee"}))
    await _souffler()

    # Unreal redemarre : nouvelle connexion pendant que ws1 agonise.
    ws2 = FauxWS()
    tache2 = asyncio.create_task(s._connexion(ws2))
    await _souffler()

    erreurs = []
    if not ws1.fermee:
        erreurs.append("l'ancienne connexion n'a pas ete fermee")
    await tache1   # son finally ne doit PAS toucher la session de ws2

    ws2.pousser(json.dumps({"evenement": "presence.detectee"}))
    await _souffler()

    if not s._occupe.locked():
        erreurs.append("le finally de l'ancienne socket a libere la session neuve")
    if s.pipeline.intros != 2:
        erreurs.append(f"{s.pipeline.intros} intro(s) au lieu de 2")
    if "session.demarree" not in ws2.evenements():
        erreurs.append("pas de session.demarree sur la nouvelle connexion")

    ws2.pousser(_FIN)
    await tache2
    if s._occupe.locked():
        erreurs.append("verrou encore tenu apres la fermeture de la connexion active")
    return erreurs


async def cas_deconnexion_brutale() -> list[str]:
    """La connexion tombe en pleine session : tout est libere."""
    s = fabriquer_serveur()
    ws = FauxWS()
    tache = asyncio.create_task(s._connexion(ws))

    ws.pousser(json.dumps({"evenement": "presence.detectee"}))
    await _souffler()
    ws.pousser(_FIN)   # crash d'Unreal : ni presence.perdue ni session.reset
    await tache

    erreurs = []
    if s._occupe.locked():
        erreurs.append("verrou encore tenu apres la deconnexion")
    if s._ws_actif is not None:
        erreurs.append("la connexion morte est restee active")
    return erreurs


CAS = [
    ("trame hors session ignoree", cas_trame_hors_session),
    ("session nominale : intro puis tour", cas_session_nominale),
    ("file bornee : un tour en attente au plus", cas_file_bornee),
    ("tour fantome annule par presence.perdue", cas_tour_fantome),
    ("double connexion : la derniere gagne, la session survit", cas_double_connexion),
    ("deconnexion brutale : session liberee", cas_deconnexion_brutale),
]


def main() -> int:
    echecs = 0
    for intitule, cas in CAS:
        erreurs = asyncio.run(cas())
        if erreurs:
            echecs += 1
            print(f"  ECHEC  {intitule}")
            for e in erreurs:
                print(f"         - {e}")
        else:
            print(f"  ok     {intitule}")

    print()
    if echecs:
        print(f"  {echecs} cas en echec sur {len(CAS)}")
        return 1
    print(f"  {len(CAS)} cas valides")
    return 0


if __name__ == "__main__":
    sys.exit(main())
