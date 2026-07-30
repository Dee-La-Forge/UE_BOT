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
│  Capture micro + VAD (Silero)              (conserve) │
│  AnimBP corps + emotions                   (conserve) │
│  Audio2Face (NV ACE, local) : trames audio            │
│    → voix + visage anime          (patch UE 5.7 —     │
│                                    voir patches/)     │
└──────────┬────────────────────────────────────────────┘
           │ WebSocket localhost (contrat : docs/CONTRAT-EVENEMENTS.md)
┌──────────▼──────────────────────┐
│  STT   faster-whisper (GPU)     │
│  LLM   llama.cpp + GBNF         │
│  TTS   Piper (CPU, 22 050 Hz)   │
└─────────────────────────────────┘
```

> Le lipsync ne passe plus par NeuroSync ni par les visemes MHF_* : les deux
> chemins sont morts (`docs/LIPSYNC-DECISION.md`). Audio2Face anime le
> visage cote Unreal, a partir des trames audio du sidecar.

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
| Audit de l'existant | ✅ `docs/AUDIT-EXISTANT.md` |
| Depot + versionnement (LFS) | ✅ |
| Specs materielles de la borne | ✅ `docs/DIMENSIONNEMENT.md` |
| Narrative Design Convai | ✅ `docs/NARRATIVE-DESIGN.md` |
| Sidecar STT→LLM→TTS | ✅ **694 ms** jusqu'au premier son |
| Lipsync local | ✅ **Audio2Face** (NV ACE local, patch UE 5.7 — `patches/`) ; NeuroSync et visemes MHF_* abandonnes (`docs/LIPSYNC-DECISION.md`) |
| Projet Unreal 5.7 + C++ | ✅ compile et lie |
| Migration des assets | ✅ 3,4 Go, verifie par comptage |
| **Parite avec l'ancien projet** | ✅ **la borne tourne a l'identique** |
| Bascule vers le sidecar | ✅ la borne tient une conversation entiere (LiDAR → STT → LLM → TTS → verdict) |
| Licences tierces | ⬜ voir `THIRD-PARTY.md` — **statuer sur Qwen2.5-3B avant exploitation** |
| Micro sur la borne | ⬜ aucun peripherique de capture |
| Test thermique 2 h | ⬜ a mener une fois la pile complete |

### Le jalon de parite

La borne fonctionne dans le nouveau projet exactement comme dans l'ancien —
LiDAR, declencheur narratif, MetaHuman, decor, VAD. Cette etape n'etait pas
cosmetique : elle fournit une **base de comparaison**. Tout ce qui cassera
desormais sera imputable a la nouvelle architecture, pas au demenagement.

> ⚠️ La cle API Convai a ete retiree des fichiers de config, mais elle
> reste lisible dans l'**historique git d'un depot public** : elle doit etre
> consideree comme compromise. **La revoquer/regenerer sur le dashboard
> Convai** — apres avoir fini l'export du personnage si elle en conditionne
> l'acces.

## Reste a recuperer depuis Convai

Le graphe narratif est sauve, mais **le personnage ne l'est pas encore**.
Restent a exporter depuis les autres onglets du dashboard
(Character ID `080151a8-4885-11f1-a397-42010a7be02e`, compte
`abo-frontieres`) :

- **backstory / personnalite** de l'agent ;
- **knowledge bank** eventuelle ;
- **reglages de voix**, pour choisir une voix locale approchante.

Tant que ce n'est pas fait, `sidecar/scenario/agent.yaml` porte une persona
**deduite du ton des objectives**, pas la vraie.

## Structure du depot

```
docs/        cadrage, audit, architecture, scenario
sidecar/     service IA local (STT / LLM / TTS)
unreal/      projet Unreal Engine
benchmarks/  mesures de latence (versionnees volontairement : c'est
             l'objet du prototype)
patches/     journaux de portage des plugins non versionnes
             (NV ACE 2.5.0rc3 → UE 5.7)
screenshoot/ captures d'ARCHIVE du dashboard Convai — narrative design et
             graphes du personnage, seule trace avant resiliation. Ne pas
             supprimer lors d'un nettoyage.
```

## Licence — point de vigilance

**NeuroSync** est gratuit (MIT) pour les entites sous 1 M$ de CA annuel, et
requiert une **licence commerciale au-dela**. Pour une installation museale,
clarifier qui est l'entite exploitante (studio ou musee) avant de batir
dessus.
