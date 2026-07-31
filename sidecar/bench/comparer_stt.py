"""Compare des moteurs de reconnaissance vocale sur le MEME audio.

Trois etapes, dans cet ordre :

    python bench/comparer_stt.py enregistrer   # sur la BORNE, avec son micro
    python bench/comparer_stt.py whisper       # transcrit avec faster-whisper
    python bench/comparer_stt.py rapport       # met les moteurs face a face

Pourquoi ce banc existe : le STT est le premier maillon, et une erreur
faite ici ne se rattrape jamais — l'agent repond a du charabia sans que
rien ne signale l'anomalie. Le projet a deja paye ce prix en abandonnant
whisper small, qui rendait « S'attu. » ou « pandinvestigation » sur des
enonces courts.

CE QUI SE MESURE, ET CE QUI DECIDE

Le taux d'erreur de mots (WER) est utile mais ne suffit pas. Un WER de 30 %
sur « Je viens de Lyon » peut donner « Je viens de Rion » — inexploitable —
ou « je viens de lyon » — parfait apres normalisation. On releve donc AUSSI
le verdict binaire qui compte pour la borne : la transcription est-elle
EXPLOITABLE ? C'est un jugement humain, et le rapport le demande
explicitement plutot que de le deduire d'un chiffre.

LE CORPUS DOIT ETRE ENREGISTRE, PAS SYNTHETISE

Tentant, et faux : passer du Piper dans les deux moteurs. La voix de
synthese est trop propre — articulation parfaite, aucun bruit de salle,
aucune hesitation. Les deux moteurs y reussiraient, et le cas qui separe
justement les modeles (enonce court, prononce vite, dans une salle de
musee) n'apparaitrait jamais. Il faut de vraies voix, sur le vrai micro,
dans le vrai lieu.
"""

from __future__ import annotations

import json
import sys
import time
import unicodedata
import wave
from pathlib import Path

RACINE = Path(__file__).resolve().parents[1]
CORPUS = RACINE / "bench" / "corpus_stt"
TAUX = 16000


# -- Le corpus -----------------------------------------------------------
#
# Domine par les enonces COURTS : c'est la que les modeles se separent, et
# c'est ce qu'un visiteur repond reellement a un garde-frontiere. Les
# phrases longues sont la pour contraste, pas pour faire nombre.
#
# Les cas adverses sont choisis d'apres des echecs REELS releves au journal
# (voir sidecar/config.yaml, note du 29/07/2026).
PHRASES: list[tuple[str, str]] = [
    # --- Tres court : un a trois mots. Le cas critique. ---
    ("court01", "Oui."),
    ("court02", "Non."),
    ("court03", "Paris."),
    ("court04", "Une semaine."),
    ("court05", "Ma soeur."),
    ("court06", "Rien du tout."),
    ("court07", "Je ne sais pas."),
    ("court08", "Aucune idee."),

    # --- Identite : noms propres, le point faible connu des petits modeles ---
    ("nom01", "Claire Vasseur."),
    ("nom02", "Je m'appelle Viktor Kellenberger."),
    ("nom03", "Meddy Boukhedouma."),

    # --- Chiffres et dates : l'autre point faible ---
    ("chiffre01", "Trois jours."),
    ("chiffre02", "Le douze mars deux mille vingt-six."),
    ("chiffre03", "J'ai quarante-deux ans."),

    # --- Reponses normales d'un visiteur ---
    ("normal01", "Je viens de Lyon."),
    ("normal02", "Je vais rendre visite a ma soeur."),
    ("normal03", "Je transporte seulement une valise de vetements."),
    ("normal04", "Je suis professeure de mathematiques."),
    ("normal05", "C'est la premiere fois que je viens ici."),

    # --- Adverses : hesitation, reprise, phrase inachevee ---
    ("adverse01", "Euh... je crois que oui."),
    ("adverse02", "Non, enfin, si, mais pas vraiment."),
    ("adverse03", "Ca depend de ce que vous entendez par la."),
    ("adverse04", "Je prefere ne pas repondre."),

    # --- Long, pour contraste ---
    ("long01", "Je viens rendre visite a ma soeur qui habite ici depuis "
               "deux ans, et je compte rester une semaine environ."),
    ("long02", "Ce que je transporte ne vous regarde absolument pas, "
               "et je n'ai pas l'intention de m'expliquer davantage."),
]


# -- Normalisation et WER ------------------------------------------------

def normaliser(t: str) -> list[str]:
    """Reduit a ce qui compte : les mots, sans casse ni ponctuation.

    Les accents sont RETIRES. Un moteur qui rend « mathematiques » au lieu
    de « mathematiques » n'a pas commis d'erreur de reconnaissance, et le
    TTS ne fera pas la difference — compter cela comme une faute
    surestimerait le taux d'erreur sans rien dire d'utile.
    """
    t = unicodedata.normalize("NFD", t.lower())
    t = "".join(c for c in t if unicodedata.category(c) != "Mn")
    return [m for m in "".join(
        c if c.isalnum() or c.isspace() else " " for c in t).split() if m]


def wer(reference: str, hypothese: str) -> float:
    """Taux d'erreur de mots, par distance d'edition. 0 = parfait."""
    r, h = normaliser(reference), normaliser(hypothese)
    if not r:
        return 0.0 if not h else 1.0
    # Levenshtein sur les mots, une seule ligne en memoire.
    precedente = list(range(len(h) + 1))
    for i, mot_r in enumerate(r, 1):
        courante = [i]
        for j, mot_h in enumerate(h, 1):
            courante.append(min(
                precedente[j] + 1,           # suppression
                courante[j - 1] + 1,         # insertion
                precedente[j - 1] + (mot_r != mot_h),   # substitution
            ))
        precedente = courante
    return precedente[-1] / len(r)


# -- Etape 1 : enregistrer le corpus -------------------------------------

def enregistrer() -> int:
    """Capte chaque phrase au micro. A FAIRE SUR LA BORNE."""
    try:
        import sounddevice as sd
        import numpy as np
    except ImportError:
        print("  sounddevice manquant : pip install sounddevice")
        return 2

    CORPUS.mkdir(parents=True, exist_ok=True)
    print(f"  peripherique d'entree : {sd.query_devices(kind='input')['name']}")
    print("  Entree = enregistrer, 's' = passer, 'q' = quitter.\n")

    for cle, texte in PHRASES:
        chemin = CORPUS / f"{cle}.wav"
        if chemin.exists():
            print(f"  [deja fait] {cle}")
            continue

        rep = input(f"  {cle:12} « {texte} »  > ").strip().lower()
        if rep == "q":
            break
        if rep == "s":
            continue

        print("     ... parlez (Entree pour arreter)")
        morceaux = []
        flux = sd.InputStream(samplerate=TAUX, channels=1, dtype="int16",
                              callback=lambda d, f, t, s: morceaux.append(d.copy()))
        with flux:
            input()
        if not morceaux:
            print("     rien capte")
            continue

        pcm = np.concatenate(morceaux)
        with wave.open(str(chemin), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(TAUX)
            w.writeframes(pcm.tobytes())
        print(f"     {len(pcm)/TAUX:.2f} s -> {chemin.name}")

    # La reference vit AVEC les enregistrements : sans elle, un corpus
    # retrouve six mois plus tard ne se compare a rien.
    (CORPUS / "reference.json").write_text(
        json.dumps({c: t for c, t in PHRASES}, ensure_ascii=False, indent=2),
        encoding="utf-8")
    print(f"\n  corpus dans {CORPUS}")
    return 0


# -- Etape 2 : transcrire avec faster-whisper ----------------------------

# Les configurations a comparer. « medium/cuda » est celle de la borne ;
# les autres servent a situer Riva par rapport a un eventail connu.
CONFIGS_WHISPER = [
    ("base-cpu",    dict(modele="base",   peripherique="cpu",  type_calcul="int8")),
    ("small-cuda",  dict(modele="small",  peripherique="cuda", type_calcul="float16")),
    ("medium-cuda", dict(modele="medium", peripherique="cuda", type_calcul="float16")),
]


def transcrire_whisper() -> int:
    import numpy as np
    import yaml
    sys.path.insert(0, str(RACINE))
    from src.stt import Transcripteur

    fichiers = sorted(CORPUS.glob("*.wav"))
    if not fichiers:
        print(f"  corpus vide : lancer d'abord « enregistrer » ({CORPUS})")
        return 2

    base = yaml.safe_load((RACINE / "config.yaml").read_text(encoding="utf-8"))["stt"]
    resultats: dict[str, dict] = {}

    for nom, surcharge in CONFIGS_WHISPER:
        cfg = dict(base) | surcharge
        print(f"\n  === {nom} ===")
        try:
            t0 = time.perf_counter()
            moteur = Transcripteur(cfg)
            print(f"      chargement : {(time.perf_counter()-t0)*1000:.0f} ms")
        except Exception as e:
            print(f"      INDISPONIBLE : {type(e).__name__} — {e}")
            continue

        for f in fichiers:
            with wave.open(str(f), "rb") as w:
                pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
                taux = w.getframerate()
            audio = pcm.astype(np.float32) / 32768.0

            t0 = time.perf_counter()
            texte = moteur.transcrire(audio, taux)
            ms = (time.perf_counter() - t0) * 1000.0

            resultats.setdefault(f.stem, {})[nom] = {"texte": texte, "ms": ms}
            print(f"      {f.stem:12} {ms:6.0f} ms  « {texte} »")

    sortie = CORPUS / "resultats_whisper.json"
    sortie.write_text(json.dumps(resultats, ensure_ascii=False, indent=2),
                      encoding="utf-8")
    print(f"\n  ecrit : {sortie}")
    return 0


# -- Etape 3 : le rapport ------------------------------------------------

def rapport() -> int:
    ref_f = CORPUS / "reference.json"
    if not ref_f.exists():
        print("  reference.json manquant : lancer « enregistrer »")
        return 2
    reference = json.loads(ref_f.read_text(encoding="utf-8"))

    moteurs: dict[str, dict] = {}
    for f, etiquette in ((CORPUS / "resultats_whisper.json", "whisper"),
                         (CORPUS / "resultats_riva.json", "riva")):
        if f.exists():
            for cle, par_moteur in json.loads(f.read_text(encoding="utf-8")).items():
                for nom, d in par_moteur.items():
                    moteurs.setdefault(nom, {})[cle] = d
        else:
            print(f"  (absent : {f.name} — cote {etiquette} non mesure)")

    if not moteurs:
        print("  rien a comparer")
        return 2

    noms = sorted(moteurs)
    print(f"\n{'=' * 78}")
    print("  WER par enonce (0.00 = parfait)")
    print(f"{'=' * 78}")
    print("  {:12} {}".format("enonce", "  ".join(f"{n:>14}" for n in noms)))

    totaux = {n: [] for n in noms}
    for cle in sorted(reference):
        ligne = f"  {cle:12} "
        for n in noms:
            d = moteurs[n].get(cle)
            if d is None:
                ligne += f"{'--':>14}  "
            else:
                e = wer(reference[cle], d["texte"])
                totaux[n].append(e)
                ligne += f"{e:>14.2f}  "
        print(ligne)

    print(f"{'-' * 78}")
    for n in noms:
        v = totaux[n]
        if not v:
            continue
        lat = [moteurs[n][c]["ms"] for c in moteurs[n] if "ms" in moteurs[n][c]]
        courts = [wer(reference[c], moteurs[n][c]["texte"])
                  for c in moteurs[n] if c.startswith(("court", "nom", "chiffre"))]
        print(f"  {n:>14} : WER global {sum(v)/len(v):.2f} | "
              f"WER enonces courts {sum(courts)/len(courts) if courts else 0:.2f} | "
              f"latence mediane {sorted(lat)[len(lat)//2]:.0f} ms" if lat else "")

    print(f"\n{'=' * 78}")
    print("  LE CHIFFRE QUI DECIDE est « WER enonces courts ».")
    print("  Un WER global flatteur peut masquer un effondrement sur les")
    print("  reponses d'un ou deux mots — qui sont l'essentiel de ce qu'un")
    print("  visiteur dit reellement.")
    print("\n  A RELIRE A LA MAIN malgre les chiffres : ouvrir les JSON et")
    print("  juger si chaque transcription est EXPLOITABLE. « Rion » pour")
    print("  « Lyon » et « lyon » pour « Lyon » ont un WER voisin et des")
    print("  consequences opposees.")
    print(f"{'=' * 78}")
    return 0


ETAPES = {"enregistrer": enregistrer, "whisper": transcrire_whisper, "rapport": rapport}

if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ETAPES:
        print(__doc__)
        print(f"  etapes : {', '.join(ETAPES)}")
        raise SystemExit(2)
    raise SystemExit(ETAPES[sys.argv[1]]())
