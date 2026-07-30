"""Synthese vocale — Piper ou Chatterbox, au choix.

Deux moteurs, meme interface (`synthetiser` -> `Parole`), selectionnes par
`tts.moteur` dans config.yaml :

  piper       ~60 Mo, CPU, plus rapide que le temps reel. Sobre et sur,
              mais la voix fr_FR-siwis est mecanique — « trop robotique »
              pour le personnage, releve a l'ecoute le 30/07/2026.
  chatterbox  350 M parametres, GPU de preference. Voix nettement plus
              incarnee, et surtout CLONAGE : dix secondes d'echantillon
              suffisent a donner au garde une voix choisie. Licence MIT.

Le lipsync ne passe PAS par ici : il est assure par Audio2Face, cote
Unreal, a partir des trames audio (docs/LIPSYNC-DECISION.md).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path

import numpy as np

_log = logging.getLogger("sidecar.tts")


@dataclass
class Parole:
    """Une phrase synthetisee."""

    pcm: np.ndarray   # float32 mono, dans [-1, 1]
    taux: int


# Prosodie par emotion : (debit, volume).
#
# Piper n'a AUCUN conditionnement d'emotion — c'est un VITS, il dit le
# texte, point. Mais il expose le debit et le volume, et le LLM decide
# deja d'une emotion a chaque replique. Faute de pouvoir la jouer, on la
# fait au moins ENTENDRE : un garde en colere parle plus vite et plus
# fort, un garde qui doute ralentit.
#
# Ce n'est pas du jeu d'acteur, et il ne faut pas le vendre comme tel.
# C'est ce qui evite qu'un visage furieux parle d'une voix egale — la
# discordance relevee a l'ecoute le 30/07/2026, une fois les emotions
# rendues au visage par Audio2Face.
#
# Les ecarts sont VOLONTAIREMENT faibles (±10 %) : au-dela, Piper
# deforme les voyelles et l'on entend le trucage.
PROSODIE = {
    "Angry":     (0.92, 1.10),   # plus vite, plus fort
    "Concerned": (1.08, 0.97),   # plus lent : le doute pese
    "Happy":     (0.97, 1.03),
    "Stare":     (1.00, 1.00),   # le defaut de la persona : neutre, tenu
    "Neutral":   (1.00, 1.00),
}


def en_pcm16(audio: np.ndarray) -> bytes:
    """Convertit en PCM16 — le format des trames du contrat."""
    return (np.clip(audio, -1.0, 1.0) * 32767).astype(np.int16).tobytes()


class SynthetiseurPiper:
    """Piper 1.6, sur CPU."""

    def __init__(self, config: dict, racine: Path):
        # Fonctionnalite retiree : refuser franchement vaut mieux que de
        # replonger en silence dans un chemin mort (le patch ONNX exigeait
        # en prime le paquet `onnx`, absent de requirements.txt).
        if config.get("phonemes_pour_repli"):
            raise RuntimeError(
                "tts.phonemes_pour_repli : fonctionnalite retiree — le "
                "lipsync vient d'Audio2Face, cote Unreal "
                "(docs/LIPSYNC-DECISION.md). Supprimer la cle."
            )

        from piper import PiperVoice, SynthesisConfig

        nom = config.get("voix", "fr_FR-siwis-medium")
        modele = racine / "models" / "piper" / f"{nom}.onnx"
        if not modele.exists():
            raise FileNotFoundError(
                f"Voix Piper introuvable : {modele}\n"
                f"Lancer scripts/telecharger.sh pour recuperer les modeles."
            )

        self._voix = PiperVoice.load(str(modele))
        self.taux = self._voix.config.sample_rate

        self._SynthesisConfig = SynthesisConfig
        self._vitesse = float(config.get("vitesse", 1.0))
        self._volume = float(config.get("volume", 1.0))
        self._prosodie = bool(config.get("prosodie_par_emotion", True))
        _log.info("TTS Piper : %s a %d Hz%s", nom, self.taux,
                  ", prosodie par emotion" if self._prosodie else "")

    def _reglages(self, emotion: str):
        debit, volume = PROSODIE.get(emotion, (1.0, 1.0)) if self._prosodie else (1.0, 1.0)
        vitesse = self._vitesse / debit
        return self._SynthesisConfig(
            # length_scale > 1 ralentit ; on inverse pour raisonner en vitesse.
            length_scale=(1.0 / vitesse) if vitesse != 1.0 else None,
            volume=self._volume * volume,
        )

    def synthetiser(self, texte: str, emotion: str = "Neutral") -> Parole:
        morceaux = list(self._voix.synthesize(texte, syn_config=self._reglages(emotion)))
        if not morceaux:
            return Parole(pcm=np.zeros(0, dtype=np.float32), taux=self.taux)

        pcm = np.concatenate([m.audio_float_array for m in morceaux]).astype(np.float32)
        return Parole(pcm=pcm, taux=morceaux[0].sample_rate)

    @staticmethod
    def en_pcm16(audio: np.ndarray) -> bytes:
        return en_pcm16(audio)


class SynthetiseurChatterbox:
    """Chatterbox — voix incarnee, et clonage a partir d'un extrait.

    Deux variantes, selon `tts.moteur` :

      chatterbox-multi  ChatterboxMultilingualTTS. 23 langues, dont le
                        FRANCAIS, choisi par `language_id`. C'est la
                        variante qu'il faut ici.
      chatterbox-turbo  ChatterboxTurboTTS. Decodeur distille, donc plus
                        rapide — mais ANGLOPHONE : il lit le francais
                        avec l'accent americain, entendu a l'essai du
                        30/07/2026. Ne vaut qu'avec un echantillon de
                        voix francaise, dont il reprend l'accent.

    Le modele se telecharge depuis Hugging Face au premier lancement
    (~1 Go), puis vit dans le cache local.

    `echantillon_voix` designe un WAV de dix secondes environ : c'est la
    voix que l'agent prendra. Sans lui, la voix par defaut du modele.
    """

    def __init__(self, config: dict, racine: Path, turbo: bool = False):
        import torch

        self._turbo = turbo
        self._langue = config.get("langue", "fr")

        peripherique = config.get("peripherique", "auto")
        if peripherique == "auto":
            peripherique = "cuda" if torch.cuda.is_available() else "cpu"

        _log.info("TTS Chatterbox %s : chargement sur %s...",
                  "turbo" if turbo else "multilingue", peripherique)

        def charger(dev: str):
            if turbo:
                from chatterbox.tts_turbo import ChatterboxTurboTTS
                return ChatterboxTurboTTS.from_pretrained(device=dev)
            from chatterbox.mtl_tts import ChatterboxMultilingualTTS, SUPPORTED_LANGUAGES
            if self._langue not in SUPPORTED_LANGUAGES:
                raise RuntimeError(
                    f"tts.langue {self._langue!r} non prise en charge "
                    f"({', '.join(SUPPORTED_LANGUAGES)})"
                )
            return ChatterboxMultilingualTTS.from_pretrained(device=dev)

        try:
            self._modele = charger(peripherique)
        except RuntimeError:
            raise   # langue invalide : ce n'est pas au repli de la rattraper
        except Exception:
            if peripherique == "cpu":
                raise
            # Meme philosophie que le STT : une borne lente vaut mieux
            # qu'une borne muette. Sur un GPU deja plein — le rendu Unreal
            # prend l'essentiel des 6 Go d'une 1060 — le chargement echoue,
            # et le CPU reste une sortie honorable.
            _log.exception("chargement Chatterbox impossible sur %s — repli CPU",
                           peripherique)
            peripherique = "cpu"
            self._modele = charger("cpu")

        self.taux = int(self._modele.sr)

        # L'echantillon a cloner. Un chemin relatif part de la racine du
        # sidecar, pour que config.yaml reste lisible.
        self._echantillon = None
        if (nom := config.get("echantillon_voix")):
            chemin = Path(nom)
            if not chemin.is_absolute():
                chemin = racine / chemin
            if not chemin.exists():
                raise FileNotFoundError(
                    f"tts.echantillon_voix introuvable : {chemin}\n"
                    "Un WAV d'une dizaine de secondes de la voix a donner "
                    "a l'agent — ou retirer la cle pour la voix par defaut."
                )
            self._echantillon = str(chemin)

        # 0 = diction neutre. Monter donne un jeu plus appuye ; un
        # garde-frontiere n'a pas a surjouer.
        self._exageration = float(config.get("exageration", 0.0))
        self._temperature = float(config.get("temperature", 0.8))

        _log.info("TTS Chatterbox pret sur %s a %d Hz%s%s",
                  peripherique, self.taux,
                  "" if turbo else f", langue {self._langue}",
                  f", voix clonee de {Path(self._echantillon).name}"
                  if self._echantillon else "")

    def synthetiser(self, texte: str, emotion: str = "Neutral") -> Parole:
        # Chatterbox, lui, SAIT jouer : `exaggeration` monte l'intensite du
        # jeu. On l'ouvre pour les emotions marquees, et on le laisse a
        # zero pour Stare — le defaut de la persona est une absence
        # d'expression tenue, pas une emotion de plus.
        appui = {"Angry": 0.6, "Concerned": 0.35, "Happy": 0.4}.get(emotion, 0.0)
        parametres = dict(
            audio_prompt_path=self._echantillon,
            exaggeration=max(self._exageration, appui),
            temperature=self._temperature,
        )
        # La variante multilingue EXIGE la langue ; la turbo ne la connait
        # pas (elle n'en parle qu'une).
        if not self._turbo:
            parametres["language_id"] = self._langue

        onde = self._modele.generate(texte, **parametres)
        # generate() rend un tenseur (1, N) sur le peripherique du modele.
        pcm = onde.detach().cpu().numpy().astype(np.float32).reshape(-1)
        return Parole(pcm=pcm, taux=self.taux)

    @staticmethod
    def en_pcm16(audio: np.ndarray) -> bytes:
        return en_pcm16(audio)


def _chatterbox_multi(config: dict, racine: Path):
    return SynthetiseurChatterbox(config, racine, turbo=False)


def _chatterbox_turbo(config: dict, racine: Path):
    return SynthetiseurChatterbox(config, racine, turbo=True)


MOTEURS = {
    "piper": SynthetiseurPiper,
    "chatterbox-multi": _chatterbox_multi,
    "chatterbox-turbo": _chatterbox_turbo,
}


def Synthetiseur(config: dict, racine: Path):
    """Fabrique le moteur demande par `tts.moteur`.

    Garde le nom de l'ancienne classe : tout le pipeline l'appelle ainsi,
    et il n'a pas a savoir lequel des deux repond.
    """
    nom = config.get("moteur", "piper")
    if nom not in MOTEURS:
        raise RuntimeError(
            f"tts.moteur inconnu : {nom!r} ({' | '.join(MOTEURS)})"
        )
    return MOTEURS[nom](config, racine)
