# Dimensionnement — borne cible

## Materiel

| | |
|---|---|
| GPU | **RTX 4090 Laptop — 16 Go VRAM** |
| RAM | 64 Go |
| Stockage | 500 Go SSD |

> Il s'agit de la variante **mobile** (AD103, 16 Go), pas de la 4090 desktop
> (24 Go). Le budget VRAM et le comportement thermique en decoulent.

## Budget VRAM — le poste contraint

Les 16 Go sont partages entre le rendu et l'inference. Repartition cible :

| Poste | Budget | Note |
|---|---|---|
| Unreal + MetaHuman + Lumen SW + VSM | **5 – 6 Go** | scene fixe, 1 personnage |
| NeuroSync (inference continue) | **~1 Go** | petit transformer seq2seq |
| LLM quantifie | **4 – 6 Go** | voir arbitrage ci-dessous |
| Marge de securite | **1,5 Go** | jamais saturer : pics d'allocation = hitch ou OOM |
| **Total** | **~13,5 / 16 Go** | |

**Decision structurante : STT et TTS tournent sur CPU.** Avec 64 Go de RAM
disponibles, cela libere ~1,5 Go de VRAM pour le LLM sans penalite de
latence significative. C'est le meilleur echange possible sur cette machine.

## Choix des modeles

### LLM — arbitrage qualite / latence

| Modele | VRAM (Q4_K_M) | 1er token | Verdict |
|---|---|---|---|
| **Qwen2.5-3B-Instruct** | ~2,0 Go | ~60 ms | ✅ **defaut recommande** |
| Qwen2.5-7B-Instruct | ~4,7 Go + KV | ~110 ms | fallback qualite |

**Recommandation : demarrer en 3B.** Le personnage n'a pas besoin de
raisonnement profond — il a besoin d'une personnalite tenue et d'un verdict.
La vitesse de generation **conditionne directement** le temps jusqu'au
premier son, puisqu'on streame phrase par phrase vers le TTS. Un 3B genere
2 a 3× plus vite qu'un 7B.

Passer au 7B seulement si le 3B tient mal le personnage sur la duree.

**Verdict contraint par grammaire GBNF** : le modele est force de terminer
par un jeton structure (`VERDICT: ACCEPTE` / `REFUSE`), plus un tag
d'emotion aligne sur l'enum `E_Emotions` existante. Fini le parsing fragile
de langage naturel — le tampon devient deterministe.

### STT — CPU

`whisper.cpp`, modele **small** francais (~500 Mo RAM).
L'endpointing est **deja resolu** : le projet embarque
`RuntimeAudioImporterSileroVAD`. Le VAD detecte la fin de parole et
declenche la transcription immediatement, sur le seul segment utile.

### TTS — CPU

**Piper** (voix francaises : `fr_FR-siwis`, `fr_FR-upmc`, `fr_FR-tom`).
~50 Mo, plus rapide que le temps reel sur CPU.

Si la qualite vocale s'avere insuffisante pour le personnage, **XTTS-v2**
permet le clonage de voix et une bien meilleure prosodie — mais consomme
~2 Go de VRAM et ajoute ~300 ms. A n'envisager qu'apres mesure.

### Lipsync — GPU

**NeuroSync Local API** : audio → 61 blendshapes ARKit + 7 emotions, 60 fps,
livres a Unreal via **LiveLink** (deja active dans le projet).

Mode degrade : mapping phonemes → les 25 poses `MHF_*` existantes, si
NeuroSync est indisponible ou si le budget GPU se tend.

## Budget de latence — estimation realiste

Du moment ou le visiteur cesse de parler jusqu'au premier son de l'agent :

| Etape | Estimation |
|---|---|
| Detection de fin de parole (VAD) | 200 – 300 ms |
| STT (whisper small CPU, enonce ~3 s) | 300 – 500 ms |
| LLM — 1re phrase (3B, streaming) | ~450 ms |
| TTS — 1re phrase (Piper CPU) | 150 – 250 ms |
| NeuroSync sur cette phrase | 100 – 200 ms |
| **Total** | **≈ 1,2 – 1,8 s** |

> **Correction d'une estimation anterieure.** J'avais avance 600 ms – 1,2 s
> avant d'integrer NeuroSync : cette etape s'ajoute en serie, puisque les
> blendshapes se calculent a partir de l'audio TTS. La cible realiste est
> **1,2 – 1,8 s**, contre vraisemblablement 2,5 – 4 s aujourd'hui avec
> Convai. Le gain reste net — et surtout **deterministe**, sans jitter
> reseau.

### Leviers d'optimisation, par ordre de rendement

1. Abaisser le seuil de silence du VAD (gain direct, 100–150 ms).
2. Contraindre le LLM a ouvrir par une **phrase courte** — c'est elle qui
   declenche l'audio, la suite se genere pendant la lecture.
3. Lancer NeuroSync sur le chunk N pendant la lecture du chunk N−1
   (pipeline, pas serie).
4. Transcription speculative sur transcript partiel.

## ⚠️ Risque thermique — a valider tot

C'est un **GPU mobile**, dans un dispositif tournant potentiellement 8 h/jour.
On empile un rendu Lumen 60 fps et une inference GPU quasi continue pendant
toute la parole de l'agent.

Le projet d'origine capait deja `t.MaxFPS=60` avec la mention explicite
« moins de chaleur/usure, anti-crash session longue » — le probleme etait
donc **deja identifie avant meme d'ajouter l'IA locale**.

**Test a mener des le prototype** : charge soutenue 2 h, releve des
temperatures GPU/CPU et du framerate. Si throttling, leviers dans l'ordre :
cap a 30 fps, reduction de la resolution interne, LLM 3B au lieu de 7B,
ventilation active du meuble.

## Stockage — 500 Go

| Poste | Estimation |
|---|---|
| Ancien projet (a conserver en reference) | 33 Go |
| Nouveau projet Unreal | ~15 Go |
| Modeles (LLM + Whisper + Piper + NeuroSync) | ~10 Go |
| Windows + Unreal Engine 5.7 | ~120 Go |

Confortable, sans etre spacieux. **Garder les builds packages hors de la
machine de production** (c'etait un des defauts de l'ancien projet : 5,2 Go
de builds stockes dans le dossier projet).
