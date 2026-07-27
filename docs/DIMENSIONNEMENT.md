# Dimensionnement — borne cible

## Materiel — releve sur la machine (verifie via `nvidia-smi`)

| | |
|---|---|
| GPU | **NVIDIA GeForce RTX 3090 Ti — 24 Go VRAM** (pilote 596.36) |
| RAM | 64 Go |
| Stockage | 500 Go SSD |
| Python | 3.13 / 3.12 / 3.10 dispo — **cible 3.12** |
| Outillage | CMake present · **ffmpeg absent** |

> Cette machine **est** la borne de production (confirme).
> Une estimation initiale evoquait une RTX 4090 Laptop 16 Go : le releve
> materiel donne une **3090 Ti 24 Go desktop**. Tout ce document est calibre
> sur le materiel reellement detecte.

## Budget VRAM — confortable

24 Go partages entre rendu et inference :

| Poste | Budget | Note |
|---|---|---|
| Unreal + MetaHuman + Lumen SW + VSM | **5 – 6 Go** | scene fixe, 1 personnage |
| NeuroSync (inference continue) | **~1 Go** | petit transformer seq2seq |
| LLM quantifie 7B | **~5,5 Go** | poids + KV cache |
| Marge de securite | **2 Go** | jamais saturer : pics = hitch ou OOM |
| **Total** | **~14,5 / 24 Go** | **~9 Go de reserve** |

**La VRAM n'est plus le facteur limitant.** Ce que la reserve debloque :

- un **LLM 7B** sans compromis (voire 14B quantifie si le personnage
  l'exigeait) ;
- **XTTS-v2 sur GPU** (~2 Go) au lieu de Piper CPU, si la qualite vocale de
  Piper s'avere insuffisante — bien meilleure prosodie, clonage de voix ;
- charger **deux LLM simultanement** pendant la phase de prototypage, pour
  comparer 3B et 7B en A/B sur la meme session.

**Decision maintenue : STT et TTS demarrent sur CPU.** Non plus par
contrainte de VRAM, mais parce que les 64 Go de RAM sont sous-employes et
que cela laisse le GPU entierement au rendu et a l'inference. On rapatriera
le TTS sur GPU seulement si la mesure montre que Piper est le maillon
faible.

## Choix des modeles

### LLM — arbitrage qualite / latence

| Modele | VRAM (Q4_K_M) | 1er token | Verdict |
|---|---|---|---|
| Qwen2.5-3B-Instruct | ~2,0 Go | ~60 ms | option latence |
| **Qwen2.5-7B-Instruct** | ~5,5 Go | ~110 ms | ✅ **defaut** |

**Recommandation : demarrer en 7B**, puisque la VRAM ne contraint plus.
Meilleure tenue du personnage sur la duree d'un entretien, meilleur respect
des consignes de format.

Mais la vitesse de generation **conditionne directement** le delai avant le
premier son, puisqu'on streame phrase par phrase vers le TTS. Un 3B genere
2 a 3× plus vite. **Le bench charge donc les deux et les compare** — c'est
la mesure qui tranche, pas l'intuition. Si le 3B tient le role, il gagne.

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

La 3090 Ti est une carte desktop a **450 W de TGP** — l'une des plus
chaudes de sa generation. Le dispositif tourne potentiellement 8 h/jour, et
on empile un rendu Lumen 60 fps et une inference GPU quasi continue pendant
toute la parole de l'agent.

Le projet d'origine capait deja `t.MaxFPS=60` avec la mention explicite
« moins de chaleur/usure, anti-crash session longue » — le probleme etait
donc **deja identifie avant meme d'ajouter l'IA locale**.

Le risque n'est pas le throttling d'un chassis portable, mais
**l'accumulation thermique dans le meuble** : une 3090 Ti dissipe 450 W en
continu dans un volume souvent clos, et l'empoussierement d'un lieu public
degrade la ventilation au fil des mois.

**Test a mener des le prototype** : charge soutenue 2 h, releve des
temperatures GPU/CPU et du framerate. Si derive, leviers dans l'ordre :
cap a 30 fps, `nvidia-smi -pl` pour limiter le power target (une 3090 Ti
bridee a 300 W perd ~5 % de perfs pour ~30 % de chaleur en moins),
reduction de la resolution interne, ventilation active du meuble,
maintenance de depoussierage planifiee.

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
