"""Synthese vocale — Piper 1.6 sur CPU.

Piper est plus rapide que le temps reel sur CPU et pese ~60 Mo. Il rend la
main phrase par phrase, ce qui est exactement ce dont le pipeline a besoin.

Bonus decouvert a l'integration : Piper 1.6 expose les **phonemes avec leur
alignement temporel**. C'est precisement ce qu'il faut pour alimenter les
25 poses de visemes MHF_* deja presentes dans le plugin Convai — le lipsync
de repli ne demande donc aucun modele supplementaire.

Si la qualite vocale ne tient pas le personnage, XTTS-v2 sur GPU reste
l'alternative (~2 Go de VRAM, ~300 ms de plus). Les 24 Go disponibles le
permettent — a n'engager qu'apres mesure.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from piper import PiperVoice, SynthesisConfig

# Correspondance phoneme eSpeak -> pose de viseme du plugin Convai
# (Plugins/Convai/Content/MetaHumans/Animations/Motions/Lips/MHF_*).
# Utilise uniquement par le mode degrade, quand NeuroSync est indisponible.
PHONEME_VERS_VISEME: dict[str, str] = {
    "a": "MHF_AA", "ɑ": "MHF_AA", "ɐ": "MHF_AA", "ã": "MHF_AA",
    "e": "MHF_EE", "ɛ": "MHF_EE", "ə": "MHF_EE", "ɛ̃": "MHF_EE",
    "i": "MHF_II", "j": "MHF_II", "ɪ": "MHF_II",
    "o": "MHF_OH", "ɔ": "MHF_OH", "ɔ̃": "MHF_OH", "ø": "MHF_OH", "œ": "MHF_OH",
    "u": "MHF_OU", "w": "MHF_OU", "y": "MHF_OU", "ɥ": "MHF_OU",
    "p": "MHF_PBM", "b": "MHF_PBM", "m": "MHF_PBM",
    "f": "MHF_FV", "v": "MHF_FV",
    "t": "MHF_TD", "d": "MHF_TD",
    "k": "MHF_KG", "g": "MHF_KG", "ɡ": "MHF_KG",
    "s": "MHF_SZ", "z": "MHF_SZ",
    "ʃ": "MHF_CH", "ʒ": "MHF_CH",
    "n": "MHF_NL", "l": "MHF_NL", "ɲ": "MHF_NL", "ŋ": "MHF_NL",
    "ʁ": "MHF_RR", "r": "MHF_RR", "ʀ": "MHF_RR",
    "θ": "MHF_TH", "ð": "MHF_TH",
}

VISEME_REPOS = "MHF_None"

# Symboles qui ne sont pas des sons : marques de frontiere (^ $), accents
# toniques (ˈ ˌ), longueur (ː), et diacritiques combinants — dont le tilde
# de nasalisation, qui suit sa voyelle au lieu de la remplacer.
# Ils PROLONGENT le viseme precedent au lieu de rouvrir la bouche au repos.
PHONEMES_TRANSPARENTS = frozenset("^$ˈˌːˑ̥̬̪̯̃͡ ")


@dataclass
class Parole:
    """Une phrase synthetisee, avec de quoi animer les levres."""

    pcm: np.ndarray                       # float32 mono, dans [-1, 1]
    taux: int
    phonemes: list[str] = field(default_factory=list)
    visemes: list[tuple[str, float, float]] = field(default_factory=list)
    # visemes : (nom_pose, debut_s, fin_s) — vide si alignements indisponibles


class Synthetiseur:
    def __init__(self, config: dict, racine: Path):
        nom = config.get("voix", "fr_FR-siwis-medium")
        modele = racine / "models" / "piper" / f"{nom}.onnx"
        if not modele.exists():
            raise FileNotFoundError(
                f"Voix Piper introuvable : {modele}\n"
                f"Lancer scripts/telecharger.sh pour recuperer les modeles."
            )
        # Les alignements ne servent qu'au lipsync de repli — chemin
        # SUPPRIME (le lipsync vient d'Audio2Face, cote Unreal). Les activer
        # patche le modele ONNX en memoire au chargement et exige le paquet
        # `onnx`, absent de requirements.txt : le defaut d'une
        # fonctionnalite retiree est False, sinon toute config sans la cle
        # (gabarit d'une seconde borne) replantait le sidecar au demarrage.
        self._alignements = bool(config.get("phonemes_pour_repli", False))

        self._voix = PiperVoice.load(str(modele), include_alignments=self._alignements)
        self.taux = self._voix.config.sample_rate

        vitesse = config.get("vitesse", 1.0)
        self._config = SynthesisConfig(
            # length_scale > 1 ralentit ; on inverse pour raisonner en vitesse.
            length_scale=(1.0 / vitesse) if vitesse and vitesse != 1.0 else None,
            volume=config.get("volume", 1.0),
        )

    def synthetiser(self, texte: str) -> Parole:
        """Synthetise une phrase et rend l'audio plus les visemes."""
        morceaux = list(
            self._voix.synthesize(
                texte, syn_config=self._config, include_alignments=self._alignements
            )
        )
        if not morceaux:
            return Parole(pcm=np.zeros(0, dtype=np.float32), taux=self.taux)

        pcm = np.concatenate([m.audio_float_array for m in morceaux]).astype(np.float32)
        phonemes = [p for m in morceaux for p in (m.phonemes or [])]

        return Parole(
            pcm=pcm,
            taux=morceaux[0].sample_rate,
            phonemes=phonemes,
            visemes=self._visemes(morceaux),
        )

    @staticmethod
    def _visemes(morceaux) -> list[tuple[str, float, float]]:
        """Convertit les alignements de phonemes en suite de poses MHF_*.

        Piper fournit une DUREE par phoneme (`num_samples`), pas un
        horodatage : la frise se construit par cumul.

        Deux regles de lissage :
        - les symboles non sonores prolongent la pose precedente ;
        - les poses identiques consecutives fusionnent, pour ne pas
          redeclencher la meme forme de bouche sur deux phonemes voisins.
        """
        suite: list[tuple[str, float, float]] = []
        curseur = 0.0   # position absolue dans la parole, en secondes

        for m in morceaux:
            taux = m.sample_rate or 22050
            for al in (m.phoneme_alignments or []):
                duree = al.num_samples / taux
                debut, fin = curseur, curseur + duree
                curseur = fin

                if al.phoneme in PHONEMES_TRANSPARENTS:
                    if suite:
                        pose, d, _ = suite[-1]
                        suite[-1] = (pose, d, fin)   # prolonge
                    continue

                pose = PHONEME_VERS_VISEME.get(al.phoneme, VISEME_REPOS)
                if suite and suite[-1][0] == pose:
                    suite[-1] = (pose, suite[-1][1], fin)   # fusion
                else:
                    suite.append((pose, debut, fin))

        return suite

    @staticmethod
    def en_pcm16(audio: np.ndarray) -> bytes:
        """Convertit en PCM16 — format attendu par NeuroSync et Unreal."""
        return (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16).tobytes()
