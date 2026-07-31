"""Joue des entretiens complets et verifie les regles du Narrative Design.

Ce que ce banc apporte, et qui manquait : jusqu'ici, la conformite du
dialogue se jugeait A L'OREILLE, une session a la fois. Les regles du
personnage — plancher et plafond de questions, vouvoiement, interdiction
de demander un document, une seule question par tour — n'etaient verifiees
par rien. Les defauts se sont donc decouverts en essai reel, tard : l'agent
accusant le visiteur de mentir avant qu'il ait parle (31/07/2026), ou
reprochant une « incoherence dans vos propos precedents » alors qu'il n'y
avait aucun propos precedent.

LE VISITEUR PARLE VRAIMENT. Ses repliques sont synthetisees par Piper et
envoyees en PCM16 16 kHz, comme le ferait Unreal. La transcription passe
donc par Whisper, et le banc traverse toute la chaine — y compris les
erreurs de STT, qui font partie de ce qu'on veut mesurer : un entretien
n'est valide que s'il tient malgre elles.

    python bench/dialogue_test.py                 # tous les profils
    python bench/dialogue_test.py cooperatif      # un seul

Le sidecar et llama.cpp doivent tourner.
"""

from __future__ import annotations

import asyncio
import json
import re
import sys
import time
from pathlib import Path

import numpy as np
import websockets
import yaml

RACINE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RACINE))

from src.tts import Synthetiseur   # noqa: E402

# Le WER vit dans l'autre banc et y est couvert par six cas de controle.
# Deux implementations d'une meme mesure divergent toujours : celle qui
# sert de reference dans un rapport, et celle qui sert de garde-fou ici,
# doivent etre la meme fonction. (bench/ est sur sys.path : le script y est.)
from comparer_stt import wer   # noqa: E402

URL = "ws://127.0.0.1:8765"
TAUX_ATTENDU = 16000     # ce que le sidecar donne a Whisper
MAX_TOURS = 16           # garde-fou : un entretien qui ne finit pas est un echec


# -- Profils de visiteur --------------------------------------------------
#
# Les reponses sont ECRITES, pas generees : un banc doit etre reproductible.
# Chaque profil eprouve une facette differente du scenario.
PROFILS: dict[str, list[str]] = {
    # Le cas nominal : quelqu'un qui repond correctement doit passer.
    "cooperatif": [
        "Bonjour. Je m'appelle Claire Vasseur.",
        "Je viens de Lyon.",
        "Je vais rendre visite à ma soeur, elle habite ici depuis deux ans.",
        "Je reste une semaine.",
        "Je transporte seulement une valise de vêtements.",
        "Non, rien d'autre. Aucun objet de valeur.",
        "Je suis professeure de mathématiques.",
        "Non, c'est la première fois que je viens ici.",
        "Oui, j'ai bien compris.",
        "Je n'ai rien à ajouter.",
    ],
    # Reponses vagues : l'agent doit reprendre ce qui est flou, pas enchainer.
    "evasif": [
        "Salut.",
        "Ça dépend de ce que vous entendez par là.",
        "Pas très loin d'ici.",
        "Je ne sais pas encore.",
        "Des affaires personnelles.",
        "C'est compliqué à expliquer.",
        "Peut-être quelques jours, peut-être plus.",
        "Je préfère ne pas répondre.",
        "Vous posez beaucoup de questions.",
        "Comme je vous l'ai dit.",
    ],
    # La persona prevoit ce cas : tourner la menace en derision.
    "menacant": [
        "Bonjour.",
        "Je m'appelle Viktor. Ça vous suffit.",
        "Je viens régler des comptes avec quelqu'un.",
        "Ce que je transporte ne vous regarde pas.",
        "Vous feriez mieux de me laisser passer.",
        "Vous allez le regretter.",
        "Je connais des gens ici.",
        "Ouvrez cette barrière.",
        "Je n'ai pas de temps à perdre.",
        "C'est votre dernier avertissement.",
    ],
}


# -- Le verdict doit DEPENDRE de ce que le visiteur a repondu ------------
#
# Un verdict rendu ne suffit pas : encore faut-il qu'il soit le bon. Releve
# le 31/07/2026 — les TROIS profils recevaient REFUSE, y compris le
# visiteur cooperatif, coherent et sans danger.
#
# Ce n'est pas un detail de dialogue, c'est une branche morte du scenario :
# si personne n'est jamais accepte, le tampon « accepte » et toute sa
# scenographie ne jouent jamais. La borne n'a plus qu'une fin.
#
# None = les deux verdicts se defendent, on n'affirme rien. Un visiteur
# evasif PEUT legitimement etre refuse ; c'est meme le metier de l'agent.
VERDICT_ATTENDU: dict[str, str | None] = {
    "cooperatif": "ACCEPTE",
    "evasif": None,
    "menacant": "REFUSE",
}


# -- Regles verifiees -----------------------------------------------------

DOCUMENTS = re.compile(
    r"\b(papiers?|passeports?|visas?|documents?|pi[eè]ce d'identit[ée]|"
    r"carte d'identit[ée]|justificatifs?|laissez-passer)\b", re.I)
TUTOIEMENT = re.compile(
    r"\b(tu|ton|ta|tes|toi|t'es|peux-tu|es-tu|as-tu|veux-tu)\b", re.I)
GENRE = re.compile(r"\b(monsieur|madame|mademoiselle)\b", re.I)
EMOTIONS_VALIDES = {"Stare", "Concerned", "Angry", "Happy", "Neutral"}


def normaliser(phrase: str) -> str:
    """Pour comparer deux tournures sans se laisser tromper par la casse."""
    return re.sub(r"\s+", " ", phrase.strip().lower())


class Entretien:
    """Un entretien joue, et ce qu'on peut en dire."""

    def __init__(self, profil: str):
        self.profil = profil
        self.tours: list[tuple[str, str]] = []   # (qui, texte)
        # (ce que le visiteur a dit, ce que l'agent a entendu). Sans cette
        # confrontation, une reponse a cote est indiscernable d'une reponse
        # a du charabia — et les deux ne se corrigent pas au meme endroit :
        # l'une dans le prompt, l'autre dans le STT.
        self.transcriptions: list[tuple[str, str]] = []
        self.emotions: list[str] = []
        self.verdict: str | None = None
        self.latences: list[float] = []
        self.incidents: list[str] = []

    @property
    def repliques_agent(self) -> list[str]:
        return [t for qui, t in self.tours if qui == "agent"]

    def controler(self, questions_min: int, questions_max: int) -> list[tuple[bool, str]]:
        """Confronte l'entretien aux regles. Rend (conforme, libelle)."""
        r: list[tuple[bool, str]] = []
        agent = self.repliques_agent
        # L'accueil n'est pas une question de l'interrogatoire.
        interro = agent[1:]

        questions = [t for t in interro if "?" in t]
        r.append((
            len(questions) >= questions_min,
            f"plancher : {len(questions)} question(s) posee(s), minimum {questions_min}",
        ))
        r.append((
            len(questions) <= questions_max,
            f"plafond : {len(questions)} question(s) posee(s), maximum {questions_max}",
        ))
        r.append((
            self.verdict is not None,
            f"verdict rendu : {self.verdict or 'AUCUN'}",
        ))

        attendu = VERDICT_ATTENDU.get(self.profil)
        if attendu is not None:
            r.append((
                self.verdict == attendu,
                f"verdict COHERENT avec les reponses : {self.verdict or 'AUCUN'} "
                f"(attendu {attendu} pour un visiteur « {self.profil} »)",
            ))

        fautifs = [t for t in agent if DOCUMENTS.search(t)]
        r.append((not fautifs, "aucune demande de document"
                  + (f" — trouve : {fautifs[:2]}" if fautifs else "")))

        fautifs = [t for t in agent if TUTOIEMENT.search(t)]
        r.append((not fautifs, "vouvoiement tenu"
                  + (f" — trouve : {fautifs[:2]}" if fautifs else "")))

        fautifs = [t for t in agent if GENRE.search(t)]
        r.append((not fautifs, "genre du visiteur jamais designe"
                  + (f" — trouve : {fautifs[:2]}" if fautifs else "")))

        # Une seule question par tour : le Narrative Design y tient, et deux
        # questions d'affilee rendent la reponse du visiteur inexploitable.
        multiples = [t for t in interro if t.count("?") > 1]
        r.append((not multiples, "une seule question par tour"
                  + (f" — {len(multiples)} tour(s) en portent plusieurs" if multiples else "")))

        vues: dict[str, int] = {}
        for t in interro:
            vues[normaliser(t)] = vues.get(normaliser(t), 0) + 1
        repetees = [t for t, n in vues.items() if n > 1]
        r.append((not repetees, "aucune tournure repetee"
                  + (f" — {len(repetees)} repetition(s)" if repetees else "")))

        # Le TIC D'OUVERTURE. La regle « tu n'emploies jamais deux fois la
        # meme tournure » se verifiait mal : le modele varie la fin de sa
        # phrase et repete son debut. Releve le 31/07/2026 — « vous mentez »
        # ouvrait sept repliques sur huit, sans qu'aucune paire ne soit
        # identique. On mesure donc les trois premiers mots.
        if interro:
            ouvertures: dict[str, int] = {}
            for t in interro:
                cle = " ".join(normaliser(t).split()[:3])
                ouvertures[cle] = ouvertures.get(cle, 0) + 1
            tete, n = max(ouvertures.items(), key=lambda kv: kv[1])
            part = n / len(interro)
            r.append((
                part <= 0.4,
                f"pas de tic d'ouverture : « {tete} » ouvre {n}/{len(interro)} "
                f"repliques ({part:.0%}, plafond 40%)",
            ))

        inconnues = set(self.emotions) - EMOTIONS_VALIDES
        r.append((not inconnues, "emotions dans le repertoire"
                  + (f" — inconnues : {inconnues}" if inconnues else "")))

        r.append((not self.incidents,
                  "aucun incident de transport"
                  + (f" — {self.incidents[:2]}" if self.incidents else "")))

        # CE QUE L'AGENT A ENTENDU. Sans cette mesure, tout verdict porte
        # sur ce banc est suspect : un agent qui refuse un visiteur
        # cooperatif a peut-etre raison, s'il a recu du charabia. La
        # question « le prompt est-il mauvais ? » ne se pose qu'une fois
        # celle-ci reglee.
        if self.transcriptions:
            muettes = [d for d, e in self.transcriptions if not e]
            r.append((
                not muettes,
                f"chaque reponse a ete entendue : {len(muettes)}/"
                f"{len(self.transcriptions)} non transcrite(s)",
            ))
            erreurs = [wer(dit, entendu) for dit, entendu in self.transcriptions]
            moyen = sum(erreurs) / len(erreurs)
            r.append((
                moyen <= 0.30,
                f"transcription fidele : WER moyen {moyen:.2f} "
                f"(plafond 0.30) sur {len(erreurs)} reponse(s)",
            ))
        return r


class VisiteurSimule:
    """Prete sa voix au visiteur : Piper, puis PCM16 16 kHz comme Unreal."""

    def __init__(self, tts: Synthetiseur):
        self.tts = tts

    def en_trame(self, texte: str) -> bytes:
        # PIEGE, paye le 31/07/2026 : Parole.pcm est du float32 dans [-1, 1],
        # PAS du int16. Un asarray(..., dtype=np.int16) ecrase donc tout le
        # signal a des valeurs entre -1 et 1 — une trame muette, que rien ne
        # signale. Le banc envoyait du silence et l'agent repondait
        # « Repetez. » a chaque tour, ce qui ressemblait a s'y meprendre a un
        # defaut du scenario.
        #
        # On passe donc par en_pcm16, la conversion que le sidecar utilise
        # lui-meme pour parler a Unreal : le banc emprunte exactement le
        # meme chemin que la production.
        parole = self.tts.synthetiser(texte, "Neutral")
        pcm = np.asarray(parole.pcm, dtype=np.float32)
        if parole.taux != TAUX_ATTENDU:
            # Meme reechantillonnage lineaire que stt.py : on ne cherche pas
            # a faire mieux que ce que la chaine fait deja.
            cible = int(len(pcm) * TAUX_ATTENDU / parole.taux)
            pcm = np.interp(
                np.linspace(0, len(pcm), cible, endpoint=False),
                np.arange(len(pcm)), pcm,
            ).astype(np.float32)
        return self.tts.en_pcm16(pcm)


async def jouer(profil: str, reponses: list[str], visiteur: VisiteurSimule) -> Entretien:
    e = Entretien(profil)

    async with websockets.connect(URL, max_size=4 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"evenement": "presence.detectee"}))

        i_reponse = 0
        dernier_dit = ""
        morceaux: list[str] = []
        depart = time.perf_counter()

        for _ in range(MAX_TOURS * 12):
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=90)
            except asyncio.TimeoutError:
                e.incidents.append("silence du sidecar au-dela de 90 s")
                break

            if isinstance(msg, bytes):
                continue     # l'audio ne nous interesse pas ici

            d = json.loads(msg)
            ev = d.get("evenement")

            if ev == "parole.debut":
                morceaux = []
                depart = time.perf_counter()
            elif ev == "parole.audio":
                if (t := d.get("texte")):
                    morceaux.append(t)
            elif ev == "visiteur.transcription":
                entendu = (d.get("texte") or "").strip()
                e.transcriptions.append((dernier_dit, entendu))
                if not entendu:
                    print("    entendu   (RIEN COMPRIS)")
                elif normaliser(entendu) == normaliser(dernier_dit):
                    print("    entendu   = mot pour mot")
                else:
                    print(f"    entendu   « {entendu} »")
            elif ev == "emotion":
                e.emotions.append(d.get("valeur", "?"))
            elif ev == "verdict":
                e.verdict = d.get("decision")
            elif ev == "erreur":
                e.incidents.append(f"erreur sidecar : {d.get('code')}")
            elif ev == "parole.fin":
                texte = " ".join(morceaux).strip()
                if not texte:
                    e.incidents.append("replique sans texte")
                    continue
                e.tours.append(("agent", texte))
                e.latences.append((time.perf_counter() - depart) * 1000.0)
                print(f"    AGENT   {texte}")

                if e.verdict in ("ACCEPTE", "REFUSE") or len(e.tours) > MAX_TOURS * 2:
                    break
                if i_reponse >= len(reponses):
                    e.incidents.append("le profil est a court de reponses")
                    break

                dit = reponses[i_reponse]
                i_reponse += 1
                dernier_dit = dit
                print(f"    visiteur  {dit}")
                e.tours.append(("visiteur", dit))
                # Petite pause : Unreal n'envoie jamais un segment dans la
                # milliseconde qui suit la fin d'une replique, et on ne veut
                # pas eprouver une condition que la borne ne produit pas.
                await asyncio.sleep(0.4)
                await ws.send(visiteur.en_trame(dit))

            elif ev == "session.terminee":
                break

        try:
            await ws.send(json.dumps({"evenement": "presence.perdue"}))
        except Exception:
            pass

    return e


async def principal() -> int:
    cfg = yaml.safe_load((RACINE / "config.yaml").read_text(encoding="utf-8"))
    scen = yaml.safe_load(
        (RACINE / cfg["scenario"]["fichier"]).read_text(encoding="utf-8"))
    qmin = scen["interrogatoire"].get("questions_min", 5)
    qmax = scen["interrogatoire"].get("questions_max", 10)

    voulus = sys.argv[1:] or list(PROFILS)
    inconnus = [p for p in voulus if p not in PROFILS]
    if inconnus:
        print(f"profil inconnu : {inconnus} — connus : {list(PROFILS)}")
        return 2

    visiteur = VisiteurSimule(Synthetiseur(cfg["tts"], RACINE))
    echecs = 0

    for profil in voulus:
        print(f"\n{'=' * 72}\n  PROFIL : {profil}\n{'=' * 72}")
        e = await jouer(profil, PROFILS[profil], visiteur)

        print(f"\n  -- controle des regles --")
        for ok, libelle in e.controler(qmin, qmax):
            print(f"    [{'OK ' if ok else 'NON'}] {libelle}")
            if not ok:
                echecs += 1

        if e.latences:
            print(f"    latence par replique : mediane "
                  f"{sorted(e.latences)[len(e.latences)//2]:.0f} ms, "
                  f"max {max(e.latences):.0f} ms")

    print(f"\n{'=' * 72}")
    print("  TOUT EST CONFORME" if not echecs else f"  {echecs} regle(s) violee(s)")
    return 1 if echecs else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(principal()))
