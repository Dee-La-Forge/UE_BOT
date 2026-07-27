"""Client de test — simule Unreal face au sidecar.

Rejoue une session complete du contrat d'evenements : presence detectee,
plusieurs tours de parole, verdict, sortie de zone. Verifie que la borne
recoit bien ce qu'elle attend pour declencher glitch, tampon et panneau.

    .venv\\Scripts\\python.exe -m bench.client_test
"""

from __future__ import annotations

import asyncio
import json
import sys
import time
import wave
from pathlib import Path

import numpy as np
import yaml
# API asyncio, la meme famille que le serveur : `await websockets.connect()`
# renvoie l'objet legacy, qui ne supporte pas `async with`.
from websockets.asyncio.client import connect

RACINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(RACINE))

ECHANTILLONS = ["motif", "duree", "provenance", "declaration", "hesitation",
                "motif", "duree", "provenance", "declaration", "hesitation",
                "motif", "duree"]


def charger_pcm16(chemin: Path) -> bytes:
    """Charge un WAV et le rend en PCM16 16 kHz mono — format visiteur."""
    with wave.open(str(chemin), "rb") as w:
        taux = w.getframerate()
        audio = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        if w.getnchannels() == 2:
            audio = audio.reshape(-1, 2).mean(axis=1).astype(np.int16)
    if taux != 16000:
        cible = int(len(audio) * 16000 / taux)
        audio = np.interp(
            np.linspace(0, len(audio), cible, endpoint=False),
            np.arange(len(audio)), audio.astype(np.float32),
        ).astype(np.int16)
    return audio.tobytes()


async def attendre_tour(ws, delai: float = 60.0) -> dict:
    """Consomme les evenements jusqu'a la fin de la replique de l'agent."""
    resume = {"phrases": [], "emotion": None, "verdict": None,
              "terminee": False, "visemes": 0, "octets": 0, "premier_son_ms": None}
    t0 = time.perf_counter()
    attente_binaire = False

    while True:
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=delai)
        except asyncio.TimeoutError:
            print("    ! delai depasse")
            return resume

        if isinstance(msg, bytes):
            resume["octets"] += len(msg)
            attente_binaire = False
            continue

        ev = json.loads(msg)
        nom = ev.get("evenement")

        if nom == "parole.debut":
            if resume["premier_son_ms"] is None:
                resume["premier_son_ms"] = (time.perf_counter() - t0) * 1000
        elif nom == "parole.audio":
            resume["phrases"].append(ev.get("texte", ""))
            resume["visemes"] += len(ev.get("visemes", []))
            attente_binaire = True
        elif nom == "emotion":
            resume["emotion"] = ev.get("valeur")
        elif nom == "verdict":
            resume["verdict"] = ev.get("decision")
        elif nom == "session.terminee":
            resume["terminee"] = True
            return resume
        elif nom == "parole.fin":
            if not attente_binaire:
                # La replique est close et aucune trame binaire n'est en
                # attente : le tour est fini.
                await asyncio.sleep(0.05)
                if resume["verdict"] is None:
                    return resume
        elif nom == "erreur":
            print(f"    ! erreur : {ev.get('code')}")
            return resume


async def principal() -> int:
    config = yaml.safe_load((RACINE / "config.yaml").read_text(encoding="utf-8"))
    url = f"ws://{config['serveur']['hote']}:{config['serveur']['port']}"

    dossier = RACINE / "bench" / "echantillons"
    if not (dossier / "motif.wav").exists():
        print("  Echantillons absents. Lancer : python -m bench.generer_echantillons")
        return 1

    print(f"  Connexion a {url}\n")
    try:
        connexion = connect(url, max_size=None)
    except OSError as e:
        print(f"  Sidecar injoignable : {e}")
        print("  Lancer d'abord : .venv\\Scripts\\python.exe main.py")
        return 1

    async with connexion as ws:
        # --- Le visiteur entre dans le champ du LiDAR --------------------
        await ws.send(json.dumps({"evenement": "presence.detectee"}))
        demarrage = json.loads(await ws.recv())
        print(f"  → session.demarree   avatar={demarrage.get('avatar')}")
        print("    (Unreal declenche ici GLITCH + SWITCH AVATAR)\n")

        verdict = None
        for i, nom in enumerate(ECHANTILLONS, 1):
            await ws.send(charger_pcm16(dossier / f"{nom}.wav"))
            r = await attendre_tour(ws)

            son = f"{r['premier_son_ms']:.0f} ms" if r["premier_son_ms"] else "—"
            print(f"  tour {i:>2}  [{son:>8}]  {r['emotion'] or '—':<10} "
                  f"{r['octets'] / 1024:>6.0f} Ko  {r['visemes']:>3} visemes")
            for p in r["phrases"]:
                print(f"            « {p} »")

            if r["verdict"]:
                verdict = r["verdict"]
                print(f"\n  → verdict : {verdict}")
                print(f"    (Unreal affiche le TAMPON stamp_{verdict.lower()})")
            if r["terminee"]:
                print("  → session.terminee")
                print("    (Unreal affiche le PANNEAU « quittez la zone »)\n")
                break

        # --- Le visiteur quitte le champ ---------------------------------
        await ws.send(json.dumps({"evenement": "presence.perdue"}))
        print("  → presence.perdue envoyee — retour en veille")

    print()
    if verdict:
        print(f"  Session complete jusqu'au verdict {verdict}.")
        return 0
    print("  Aucun verdict rendu — le plafond de questions n'a pas ete atteint.")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(principal()))
