"""NeuroSync — audio vers blendshapes ARKit, en local.

Transformer seq2seq : MFCC en entree, 68 valeurs par trame en sortie
(61 blendshapes ARKit + 7 emotions), a 60 fps.

Poids : `model.pth` obtenu depuis huggingface.co/convaitech/NEUROSYNC (MIT).

Les dimensions sont **deduites du checkpoint**, pas codees en dur : le
`config.py` publie annonce 4 couches alors que ce checkpoint en compte 8.
Lire les poids evite de dependre d'une configuration qui ne correspond pas.

Chaine complete :

    PCM ─► resample 88200 Hz ─► 23 MFCC + delta + delta2 = 69 dims
        ─► fenetres de 128 trames (recouvrement 16)
        ─► encodeur 8 couches ─► decodeur 8 couches (non autoregressif)
        ─► 68 valeurs/trame ─► /100 sur les 61 premieres
"""

from __future__ import annotations

import io
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

# --- Constantes du modele ------------------------------------------------
SR = 88200                      # taux d'echantillonnage attendu
FPS = 60                        # trames de sortie par seconde
DUREE_TRAME = 1.0 / FPS         # 16,67 ms
N_MFCC = 23                     # ×3 (mfcc, delta, delta2) = 69 dimensions
N_MFCC_TOTAL = N_MFCC * 3       # 69
N_AUTOCORR = 187                # coefficients d'autocorrelation retenus
# 69 + 187 = 256, la largeur attendue par le checkpoint.
#
# Le bloc d'autocorrelation porte les poids les plus uniformes de la matrice
# d'embedding (0.6496 +/- 0.015). Cette platitude n'est pas un signe de
# poids morts, comme on pourrait le croire : elle traduit simplement que
# toutes ces composantes sont de meme nature.
TAILLE_FENETRE = 128            # trames par passe
RECOUVREMENT = 16               # trames de fondu entre fenetres
N_BLENDSHAPES = 61              # ARKit ; les 7 suivantes sont des emotions
ECHELLE_BLENDSHAPES = 100.0     # le modele sort du 0-100, Unreal veut du 0-1


# =========================================================================
# Architecture
# =========================================================================

class EncodagePositionnel(nn.Module):
    """Encodage sinusoidal classique, ajoute aux embeddings."""

    def __init__(self, dim: int, max_len: int = 10000):
        super().__init__()
        pe = torch.zeros(max_len, dim)
        pos = torch.arange(max_len, dtype=torch.float).unsqueeze(1)
        facteur = torch.exp(
            torch.arange(0, dim, 2).float() * (-math.log(10000.0) / dim)
        )
        pe[:, 0::2] = torch.sin(pos * facteur)
        pe[:, 1::2] = torch.cos(pos * facteur)
        # persistent=False : la table est entierement recalculable, et le
        # checkpoint ne la contient pas. Sans ce drapeau, load_state_dict
        # en mode strict la reclamerait.
        self.register_buffer("pe", pe.unsqueeze(0), persistent=False)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.pe[:, : x.size(1)]


class AttentionMultiTete(nn.Module):
    """Attention multi-tetes. Noms des sous-modules alignes sur le checkpoint."""

    def __init__(self, dim: int, n_tetes: int):
        super().__init__()
        assert dim % n_tetes == 0, "dim doit etre divisible par n_tetes"
        self.n_tetes = n_tetes
        self.dim_tete = dim // n_tetes

        self.q_linear = nn.Linear(dim, dim)
        self.k_linear = nn.Linear(dim, dim)
        self.v_linear = nn.Linear(dim, dim)
        self.out_linear = nn.Linear(dim, dim)

    def forward(self, requete, cle, valeur) -> torch.Tensor:
        b = requete.size(0)

        def decouper(t):
            return t.view(b, -1, self.n_tetes, self.dim_tete).transpose(1, 2)

        q = decouper(self.q_linear(requete))
        k = decouper(self.k_linear(cle))
        v = decouper(self.v_linear(valeur))

        # Flash Attention quand disponible : gratuit et plus rapide.
        sortie = F.scaled_dot_product_attention(q, k, v)
        sortie = sortie.transpose(1, 2).contiguous().view(b, -1, self.n_tetes * self.dim_tete)
        return self.out_linear(sortie)


class ReseauFeedForward(nn.Module):
    def __init__(self, dim: int, dim_interne: int):
        super().__init__()
        self.linear1 = nn.Linear(dim, dim_interne)
        self.linear2 = nn.Linear(dim_interne, dim)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.linear2(F.relu(self.linear1(x)))


class CoucheEncodeur(nn.Module):
    """Post-norm : residu ajoute, PUIS normalisation."""

    def __init__(self, dim: int, n_tetes: int, dim_ff: int):
        super().__init__()
        self.self_attn = AttentionMultiTete(dim, n_tetes)
        self.ffn = ReseauFeedForward(dim, dim_ff)
        self.norm1 = nn.LayerNorm(dim)
        self.norm2 = nn.LayerNorm(dim)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.norm1(x + self.self_attn(x, x, x))
        x = self.norm2(x + self.ffn(x))
        return x


class CoucheDecodeur(nn.Module):
    def __init__(self, dim: int, n_tetes: int, dim_ff: int):
        super().__init__()
        self.self_attn = AttentionMultiTete(dim, n_tetes)
        self.multihead_attn = AttentionMultiTete(dim, n_tetes)   # attention croisee
        self.ffn = ReseauFeedForward(dim, dim_ff)
        self.norm1 = nn.LayerNorm(dim)
        self.norm2 = nn.LayerNorm(dim)
        self.norm3 = nn.LayerNorm(dim)

    def forward(self, x: torch.Tensor, memoire: torch.Tensor) -> torch.Tensor:
        x = self.norm1(x + self.self_attn(x, x, x))
        x = self.norm2(x + self.multihead_attn(x, memoire, memoire))
        x = self.norm3(x + self.ffn(x))
        return x


class Encodeur(nn.Module):
    def __init__(self, dim_entree: int, dim: int, n_couches: int, n_tetes: int, dim_ff: int):
        super().__init__()
        self.embedding = nn.Linear(dim_entree, dim)
        self.global_pos_encoder = EncodagePositionnel(dim)
        self.transformer_encoder = nn.ModuleList(
            CoucheEncodeur(dim, n_tetes, dim_ff) for _ in range(n_couches)
        )
        self.layer_norm = nn.LayerNorm(dim)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.global_pos_encoder(self.embedding(x))
        for couche in self.transformer_encoder:
            x = couche(x)
        return self.layer_norm(x)


class Decodeur(nn.Module):
    """Non autoregressif : la sortie de l'encodeur sert de requete ET de memoire.

    Une seule passe, donc pas de boucle — c'est ce qui rend l'inference rapide.
    """

    def __init__(self, dim_sortie: int, dim: int, n_couches: int, n_tetes: int, dim_ff: int):
        super().__init__()
        self.global_pos_encoder = EncodagePositionnel(dim)
        self.transformer_decoder = nn.ModuleList(
            CoucheDecodeur(dim, n_tetes, dim_ff) for _ in range(n_couches)
        )
        self.fc_output = nn.Linear(dim, dim_sortie)
        self.layer_norm = nn.LayerNorm(dim)

    def forward(self, sorties_encodeur: torch.Tensor) -> torch.Tensor:
        x = self.global_pos_encoder(sorties_encodeur)
        for couche in self.transformer_decoder:
            x = couche(x, sorties_encodeur)
        return self.fc_output(self.layer_norm(x))


class Seq2Seq(nn.Module):
    def __init__(self, encodeur: Encodeur, decodeur: Decodeur):
        super().__init__()
        self.encoder = encodeur
        self.decoder = decodeur

    def forward(self, src: torch.Tensor) -> torch.Tensor:
        return self.decoder(self.encoder(src))


# =========================================================================
# Chargement
# =========================================================================

@dataclass
class Dimensions:
    dim_entree: int
    dim_cachee: int
    dim_sortie: int
    n_couches: int
    n_tetes: int
    dim_ff: int

    def __str__(self) -> str:
        return (f"entree={self.dim_entree} cachee={self.dim_cachee} "
                f"sortie={self.dim_sortie} couches={self.n_couches} "
                f"tetes={self.n_tetes} ff={self.dim_ff}")


def deduire_dimensions(etat: dict) -> Dimensions:
    """Lit la geometrie du reseau dans les poids eux-memes.

    Plus fiable qu'un fichier de configuration : celui publie avec le projet
    annonce 4 couches, ce checkpoint en contient 8.
    """
    dim_cachee, dim_entree = etat["encoder.embedding.weight"].shape
    dim_sortie = etat["decoder.fc_output.weight"].shape[0]
    dim_ff = etat["encoder.transformer_encoder.0.ffn.linear1.weight"].shape[0]

    couches = {
        int(cle.split(".")[2])
        for cle in etat
        if cle.startswith("encoder.transformer_encoder.")
    }
    n_couches = max(couches) + 1

    # Le nombre de tetes n'est PAS inscrit dans les poids : les projections
    # q/k/v restent carrees quel qu'il soit. Un mauvais choix decoupe
    # l'attention de travers sans declencher la moindre erreur de forme.
    # Valeur tiree de la config qui accompagne ce checkpoint (8 couches,
    # entree 256) : 16 tetes de 64.
    n_tetes = 16

    return Dimensions(dim_entree, dim_cachee, dim_sortie, n_couches, n_tetes, dim_ff)


class NeuroSync:
    def __init__(self, chemin_modele: Path, peripherique: str = "cuda"):
        if not chemin_modele.exists():
            raise FileNotFoundError(f"Modele NeuroSync introuvable : {chemin_modele}")

        self.peripherique = torch.device(
            peripherique if torch.cuda.is_available() or peripherique == "cpu" else "cpu"
        )

        etat = torch.load(chemin_modele, map_location="cpu", weights_only=True)
        self.dims = deduire_dimensions(etat)

        modele = Seq2Seq(
            Encodeur(self.dims.dim_entree, self.dims.dim_cachee,
                     self.dims.n_couches, self.dims.n_tetes, self.dims.dim_ff),
            Decodeur(self.dims.dim_sortie, self.dims.dim_cachee,
                     self.dims.n_couches, self.dims.n_tetes, self.dims.dim_ff),
        )
        # strict=True : si un seul nom ou une seule forme diverge, on le saura
        # ici plutot que par une animation faciale silencieusement fausse.
        modele.load_state_dict(etat, strict=True)

        self.modele = modele.to(self.peripherique).eval()

    # -- Extraction des features -----------------------------------------

    def _features(self, pcm: np.ndarray, taux: int) -> np.ndarray | None:
        """PCM float32 -> matrice (trames, 69).

        23 MFCC + leurs derivees premiere et seconde, normalises, puis
        moyennes par paires pour retomber a 60 trames/seconde.
        """
        import librosa

        y = pcm.astype(np.float32)
        if taux != SR:
            y = librosa.resample(y, orig_sr=taux, target_sr=SR)

        crete = float(np.max(np.abs(y))) if y.size else 0.0
        if crete > 0:
            y = y / crete

        longueur_trame = int(DUREE_TRAME * SR)   # 1470
        saut = longueur_trame // 2               # 735

        if (len(y) - longueur_trame) // saut + 1 < 9:
            return None   # trop court pour etre exploitable

        mfcc = librosa.feature.mfcc(
            y=y, sr=SR, n_mfcc=N_MFCC, n_fft=longueur_trame, hop_length=saut
        )
        moyenne = mfcc.mean(axis=1, keepdims=True)
        ecart = mfcc.std(axis=1, keepdims=True)
        mfcc = (mfcc - moyenne) / (ecart + 1e-10)

        empile = np.vstack([
            mfcc,
            librosa.feature.delta(mfcc),
            librosa.feature.delta(mfcc, order=2),
        ])

        # Moyenne par paires : le saut vaut la demi-trame, on revient a 60 fps.
        n = empile.shape[1]
        paires = empile[:, : n // 2 * 2].reshape(empile.shape[0], -1, 2).mean(axis=2)
        if n % 2 == 1:
            paires = np.hstack((paires, empile[:, -1:]))

        mfcc_final = paires.T.astype(np.float32)          # (trames, 69)
        autocorr = self._autocorrelation(y, longueur_trame, saut)   # (trames, 187)

        # Les deux chaines peuvent differer d'une trame : l'autocorrelation
        # rembourre le signal avant decoupage. On aligne sur la plus courte.
        n = min(mfcc_final.shape[0], autocorr.shape[0])
        return np.hstack([mfcc_final[:n], autocorr[:n]]).astype(np.float32)

    @staticmethod
    def _autocorrelation(y: np.ndarray, longueur_trame: int, saut: int) -> np.ndarray:
        """187 coefficients d'autocorrelation par trame.

        Complement indispensable aux MFCC : c'est ce bloc qui porte la
        periodicite du signal — donc la voisement, la hauteur, l'ouverture.
        Sans lui, le modele ne dispose que de l'enveloppe spectrale et rend
        un visage inerte.
        """
        rembourrage = longueur_trame // 2
        y_pad = np.pad(y, pad_width=rembourrage, mode="reflect")

        import librosa
        trames = librosa.util.frame(y_pad, frame_length=longueur_trame, hop_length=saut)
        trames = trames - trames.mean(axis=0, keepdims=True)
        trames = trames * np.hanning(longueur_trame)[:, np.newaxis]

        coeffs = []
        milieu = longueur_trame - 1
        for trame in trames.T:
            plein = np.correlate(trame, trame, mode="full")
            # On part du decalage nul et on garde N+1 valeurs.
            retenu = plein[milieu: milieu + N_AUTOCORR + 1]
            if retenu[0] != 0:
                retenu = retenu / retenu[0]     # normalisation par l'energie
            coeffs.append(retenu)

        # (N+1, trames) puis on retire le decalage nul, qui vaut 1 partout.
        a = np.array(coeffs).T[1:, :]

        # Trames de bord parfois nulles : on recopie la voisine.
        if a.shape[1] >= 2:
            if np.all(np.abs(a[:, 0]) < 1e-7):
                a[:, 0] = a[:, 1]
            if np.all(np.abs(a[:, -1]) < 1e-7):
                a[:, -1] = a[:, -2]

        # Meme reduction par paires que les MFCC, pour retomber a 60 fps.
        n = a.shape[1]
        paires = a[:, : n // 2 * 2].reshape(a.shape[0], -1, 2).mean(axis=2)
        if n % 2 == 1:
            paires = np.hstack((paires, a[:, -1:]))

        return paires.T.astype(np.float32)

    # -- Inference --------------------------------------------------------

    @torch.no_grad()
    def _inferer_fenetre(self, fenetre: np.ndarray) -> np.ndarray:
        t = torch.from_numpy(fenetre).unsqueeze(0).to(self.peripherique)
        return self.modele(t).squeeze(0).cpu().numpy()

    @staticmethod
    def _fondu(a: np.ndarray, b: np.ndarray, recouvrement: int) -> np.ndarray:
        """Fondu lineaire entre deux fenetres, pour masquer la jointure."""
        n = min(recouvrement, len(a), len(b))
        if n == 0:
            return np.vstack((a, b))
        melange = a.copy()
        alphas = np.linspace(0.0, 1.0, n, endpoint=False).reshape(-1, 1)
        melange[-n:] = (1 - alphas) * a[-n:] + alphas * b[:n]
        return np.vstack((melange, b[n:]))

    def blendshapes(self, pcm: np.ndarray, taux: int) -> np.ndarray:
        """PCM -> tableau (trames, 68) a 60 fps.

        Colonnes 0-60 : blendshapes ARKit, ramenes dans [0, 1].
        Colonnes 61-67 : valeurs d'emotion, laissees telles quelles.
        """
        features = self._features(pcm, taux)
        if features is None:
            return np.zeros((0, self.dims.dim_sortie), dtype=np.float32)

        n_trames = features.shape[0]
        morceaux: list[np.ndarray] = []
        debut = 0

        while debut < n_trames:
            fin = min(debut + TAILLE_FENETRE, n_trames)
            fenetre = features[debut:fin]

            if fenetre.shape[0] < TAILLE_FENETRE:
                manque = TAILLE_FENETRE - fenetre.shape[0]
                # Remplissage en miroir : moins de discontinuite qu'un zero.
                rembourrage = np.pad(fenetre, ((0, manque), (0, 0)), mode="reflect")
                fenetre = np.vstack((fenetre, rembourrage[-manque:, : features.shape[1]]))

            sortie = self._inferer_fenetre(fenetre)[: fin - debut]

            if morceaux:
                morceaux.append(self._fondu(morceaux.pop(), sortie, RECOUVREMENT))
            else:
                morceaux.append(sortie)

            debut += TAILLE_FENETRE - RECOUVREMENT

        resultat = np.concatenate(morceaux, axis=0)[:n_trames]
        if resultat.ndim == 3:
            resultat = resultat.reshape(-1, resultat.shape[-1])

        # Le modele sort les blendshapes sur une echelle 0-100.
        resultat = resultat.astype(np.float32)
        resultat[:, :N_BLENDSHAPES] /= ECHELLE_BLENDSHAPES
        return resultat
