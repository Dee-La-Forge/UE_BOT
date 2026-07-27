# Couche de presentation — ce qui reste cote Unreal

Quatre dispositifs scenographiques accompagnent l'entretien. **Aucun ne
depend du cloud** : ils sont deja entierement geres dans le Blueprint et le
restent apres migration. Le sidecar ne fait que les *declencher*, via les
evenements decrits dans `CONTRAT-EVENEMENTS.md`.

## 1. Switch de personnage

Trois avatars MetaHuman se relaient : `BP_AgentGermain`, `BP_AgentLouise`,
`BP_AgentTrinity`. Selection par `RandomInteger` sur le tableau
`MetahumanClasses`, application par `SetMainCharacter` / `ActiveCharacter`
(trace `ACTIVE MH SET`), remise a zero par `RESET PERSONNAGE`.

### ⚠️ Le switch change aussi le personnage conversationnel

Releve dans les logs et les Blueprints :

| Character ID Convai | Blueprint | Occurrences |
|---|---|---|
| `080151a8-4885-11f1-a397-42010a7be02e` | BP_AgentGermain | 19 488 |
| `97630e92-20a6-11f1-936e-42010a7be02c` | BP_AgentLouise **et** BP_AgentTrinity | 7 353 |
| `66a14864-20a6-11f1-8224-42010a7be02c` | *aucun* — orphelin | 6 000 |
| — | BP_AgentGrondin, BP_AgentSmith | *aucun ID* |

Trois points a trancher :

1. **Louise et Trinity partagent le meme Character ID.** Meme persona, meme
   voix, deux visages. Intentionnel, ou copier-coller a la duplication du
   Blueprint ?
2. **`66a14864` est massivement present dans les logs mais dans aucun
   Blueprint** — vestige d'une version anterieure, a ignorer ou a exhumer.
3. **Le Narrative Design exporte ne couvre que Germain.** Celui de
   `97630e92` reste a recuperer.

### Consequence pour la stack locale

Il faut **une persona et une voix par personnage**, pas une seule :

| Avatar | Persona | Voix locale |
|---|---|---|
| Germain | `scenario/germain.yaml` | voix masculine |
| Louise / Trinity | `scenario/louise.yaml` *(a exporter)* | voix feminine |

Le fichier `scenario/agent.yaml` actuel ne porte que Germain. Le decoupage
en un fichier par personnage se fera une fois le second Narrative Design
recupere — inutile de figer une arborescence sur une seule moitie des
donnees.

## 2. Effets de Glitch

`PPM_GLITCH` (post-process), `GlitchTimeline`, `DynamicGlitchMat`
(`SetScalarParameterValue`), `Glitch_Sound_By_DyBoy`, boucle
`GlitchFXLoop` / `GlitchLoopActive`, arret par `StopGlitch`.

Role scenographique : **masquer la bascule d'avatar**. Le glitch couvre
l'instant ou le MetaHuman change d'identite, ce qui transforme une
substitution technique en effet voulu.

Le Blueprint trace `Init PPV OK` / `Error PPV` — l'initialisation du
post-process volume est un point de fragilite connu, a reprendre avec une
vraie gestion d'erreur.

## 3. Tampons accepte / refus

`WBP_Stamp` (widget), textures `stamp_accepted` / `stamp_refused`,
declenchement par `ShowStamp`, nettoyage par `Remove stamps`.

Le widget recoit une chaine `Decision` et fait un `Contains` dessus.

**Avant** : le verdict arrivait sous forme de `NarrativeSectionID`, compare
a deux UUID codes en dur dans le Blueprint
(`8836d87a-…` = refus, `9a67d152-…` = accepte).

**Apres** : le tag `[VERDICT:ACCEPTE|REFUSE]`, contraint par grammaire GBNF.
Meme resultat, sans UUID en dur, et impossible a mal former.

## 4. Panneau « quittez la zone »

`Exit_Stamp`, `SwitchToExitStamp`, `ShowExitStamp`, `IsExitStampPhase`.

Phase terminale : une fois le verdict rendu et le tampon affiche, l'agent
invite le visiteur a sortir du champ du LiDAR pour liberer la borne.
`BP_LidarManager` compte les releves d'absence (`AbsenceCount`) ; au-dela du
seuil, `ResetLidarState` et `RELOAD APRES ABANDON` reinitialisent la
session.

**Cette phase n'a jamais transite par Convai.** Le graphe narratif s'arrete
a `accepte` / `refus`. Elle est donc hors perimetre de la migration — a
preserver telle quelle.

## Machine a etats complete

```
   ┌────────────── VEILLE ◄──────────────────────────────┐
   │                  │ LiDAR : presence                  │
   │                  ▼                                   │
   │        [GLITCH + SWITCH AVATAR]                      │
   │                  │                                   │
   │                  ▼                                   │
   │             INTRO (accueil)                          │
   │                  │                                   │
   │                  ▼                                   │
   │        INTERROGATOIRE (5 a 10 questions)             │
   │                  │                                   │
   │                  ▼                                   │
   │        VERDICT ──► [TAMPON accepte | refus]          │
   │                  │                                   │
   │                  ▼                                   │
   │        SORTIE ──► [PANNEAU "quittez la zone"]        │
   │                  │                                   │
   │                  ▼                                   │
   └──── AbsenceCount >= seuil ──► RESET ─────────────────┘
                      ▲
                      │ abandon / timeout en cours de route
```

Les etapes entre crochets sont **purement Unreal**. Le sidecar ne les
execute pas : il emet les evenements qui les declenchent.
