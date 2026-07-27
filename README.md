# Projet GD — IA locale

Refonte du dispositif muséal **Garde Frontière**, avec remplacement de l'IA
conversationnelle Convai (cloud) par une **stack IA entierement locale**.

## Objectif

Supprimer la latence des allers-retours reseau, la dependance a un service
tiers et l'abonnement associe — sans perdre la qualite d'animation du
MetaHuman.

## Le dispositif

Borne interactive de musee, a but de divertissement :

```
        ┌──────────────── VEILLE ◄──────────────────────────┐
        │                    │ LiDAR : presence detectee     │
        ▼                    ▼                               │
  [absence confirmee]   ACCUEIL — l'agent prend la parole    │
        ▲                    │                               │
        │                    ▼                               │
   SORTIE_ZONE ◄─ TAMPON ◄─ VERDICT ◄─ ENTRETIEN (n questions)
   "liberez la place"   accepte/refuse    │                   │
        │                                 │ abandon /         │
        └─────────────────────────────────┴─ timeout ─────────┘
```

Un visiteur se presente devant le LiDAR, un agent MetaHuman l'interroge
quelques questions, puis l'autorise ou lui refuse l'acces au musee. Le
scenario termine, l'agent invite le visiteur a quitter la zone de detection
pour liberer la place au suivant.

## Architecture cible

```
┌─── UNREAL (60 fps garantis) ──────────────────────────┐
│  LiDAR (serie) ──► GuardSessionManager (C++)          │
│  Capture micro + VAD                       (conserve) │
│  Streaming audio                           (conserve) │
│  AnimBP corps + emotions                   (conserve) │
│  LiveLink ARKit ◄── visage            (deja active)   │
└──────────┬─────────────────────────▲──────────────────┘
           │ WebSocket localhost     │ LiveLink
┌──────────▼─────────────┐  ┌────────┴──────────────────┐
│  STT   whisper.cpp     │  │  NeuroSync Local API      │
│  LLM   quantifie + GBNF│─►│  audio → 61 ARKit + 7 emo │
│  TTS   Piper / XTTS ───┼─►│  60 fps                   │
└────────────────────────┘  └───────────────────────────┘
```

**Principe directeur : sidecar local, pas d'embarque dans Unreal.**
Le loopback coute moins d'1 ms — negligeable — et on y gagne trois choses
decisives pour une borne en autonomie :

1. si l'IA plante, Unreal survit et bascule en mode degrade ;
2. le service IA redemarre seul, sans relancer l'application ;
3. on itere sur les prompts **sans recompiler le projet**.

**Le gain de latence vient du streaming, pas du "local" en soi.** LLM token
par token → des la premiere ponctuation, la phrase part au TTS → l'audio
demarre pendant que le LLM ecrit la suite. Cible : premier son en
**600 ms – 1,2 s**.

## Etat du chantier

| Etape | Statut |
|---|---|
| Audit de l'existant | ✅ fait — voir `docs/AUDIT-EXISTANT.md` |
| Depot + versionnement | ✅ fait |
| Export du Narrative Design Convai | ⏳ **bloquant** |
| Specs materielles de la borne | ⏳ **bloquant** |
| Prototype sidecar (STT→LLM→TTS) | ⬜ a venir |
| Integration Unreal | ⬜ a venir |

## Points bloquants

**1. Exporter le Narrative Design depuis le dashboard Convai — URGENT.**
La personnalite de l'agent, ses questions, ses regles de decision et
l'enchainement des sections narratives **ne sont pas dans le depot** : ils
vivent sur les serveurs Convai, sous le Character ID
`080151a8-4885-11f1-a397-42010a7be02e`. Le jour ou l'abonnement s'arrete,
cette matiere disparait. Le code se reecrit ; le contenu conversationnel
affine au fil des sessions, non.

**2. Specs GPU / VRAM / RAM de la borne.**
Determinent tout le dimensionnement : un GPU 24 Go et un GPU 8 Go
n'admettent pas la meme architecture (taille du LLM, STT/TTS sur CPU ou
GPU, second GPU eventuel).

## Structure du depot

```
docs/        cadrage, audit, architecture, scenario
sidecar/     service IA local (STT / LLM / TTS)
unreal/      projet Unreal Engine
benchmarks/  mesures de latence
```

## Licence — point de vigilance

**NeuroSync** est gratuit (MIT) pour les entites sous 1 M$ de CA annuel, et
requiert une **licence commerciale au-dela**. Pour une installation museale,
clarifier qui est l'entite exploitante (studio ou musee) avant de batir
dessus.
