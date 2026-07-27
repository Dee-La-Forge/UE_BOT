# Projet Unreal — Garde Frontiere

Unreal Engine **5.7**. Le C++ porte la logique de session ; le Blueprint
garde la scenographie, ou il est a sa place.

## Ce qui change par rapport a l'ancien projet

L'ancien concentrait tout dans `BP_ConvaiCharacterBase` : orchestration IA,
selection d'avatar, gating micro, post-process glitch, widget tampon, etat
LiDAR, cycle de session et timers — dans un seul asset binaire, indiffable
et intestable.

| | Avant | Maintenant |
|---|---|---|
| Machine a etats | noyee dans un Blueprint | `AGuardSessionManager`, C++ explicite |
| Capteur | `BP_LidarManager`, port en dur | `ULidarPresenceComponent`, configurable |
| IA | plugin Convai, cloud | `USidecarClient`, WebSocket local |
| Diagnostic | `Print String` | `LogGardeFrontiere`, filtrable et archivable |
| Reconnexion | aucune | capteur et sidecar, automatique |

## Machine a etats

```
   ┌──────────── Veille ◄──────────────────────────────┐
   │               │ capteur : presence                 │
   │               ▼                                    │
   │           Accueil ──► [GLITCH + SWITCH AVATAR]     │
   │               │                                    │
   │               ▼                                    │
   │       Interrogatoire (5 a 10 questions)            │
   │               │                                    │
   │               ▼                                    │
   │           Verdict ──► [TAMPON accepte | refuse]    │
   │               │                                    │
   │               ▼                                    │
   │        SortieZone ──► [PANNEAU "quittez la zone"]  │
   │               │                                    │
   └── capteur : absence ───────────────────────────────┘
                   ▲
                   │ abandon / timeout a tout moment
```

Les etapes entre crochets sont **purement Blueprint**. Le C++ ne sait pas
ce qu'est un glitch : il emet `OnSessionDemarree`, `OnVerdictRendu`,
`OnDemandeSortieZone`, et la scenographie s'y branche.

On peut donc retoucher les effets sans toucher a la logique, et changer de
LLM sans rouvrir un Blueprint.

## Trois corrections apportees au capteur

`BP_LidarManager` etait sain dans son principe. Trois faiblesses corrigees :

1. **Port, vitesse et seuil etaient en dur** — desormais exposes dans le
   detail panel, modifiables sur site sans recompiler.
2. **Aucune reconnexion** : un capteur debranche condamnait la borne
   jusqu'au redemarrage. Le composant reessaie indefiniment, et libere la
   session en cours si la liaison tombe.
3. **Pas d'hysteresis a la detection** : seule l'absence etait filtree
   (`AbsenceCount`), si bien qu'un passant pouvait declencher une session.
   Il faut maintenant N releves consecutifs dans chaque sens, avec un seuil
   de sortie plus large que celui d'entree.

## Mode degrade

Une borne muette avec un visiteur plante devant est le seul echec vraiment
couteux. Toute panne du sidecar remonte par `OnRepliqueDeSecours`, et le
Blueprint fait parler l'agent malgre tout.

| Panne | Comportement |
|---|---|
| Sidecar injoignable | replique de secours, reconnexion toutes les 5 s |
| Capteur debranche | session en cours liberee, reouverture du port en boucle |
| Aucune interaction | abandon apres `DelaiAbandon` |

## Reste a faire

- [ ] Migrer les assets depuis l'ancien projet (~1,7 Go, voir ci-dessous)
- [ ] Blueprint de scenographie branche sur les evenements
- [ ] Source LiveLink ARKit pour recevoir les blendshapes du sidecar
- [ ] Capture micro + VAD vers `EnvoyerAudioVisiteur`
- [ ] Lecture des trames audio recues

### Assets a migrer

| Depuis l'ancien projet | Poids |
|---|---|
| `Content/MetaHumans/AgentGermain` | 721 Mo |
| `Content/MetaHumans/Common` | 253 Mo |
| `Plugins/Convai/Content/MetaHumans/Animations` | 696 Mo |
| `Content/GlitchFx`, `StampFx`, `Materials`, `Meshes` | 9 Mo |

**A ne pas migrer** : `AgentGrondin` (737 Mo) et `AgentSmith` (707 Mo), sans
Character ID ni reference ; `PoliceUniform` (394 Mo) ; les 18 maps de
presets d'eclairage MetaHuman.

> Le dossier `Animations` du plugin Convai est **indispensable** : il porte
> les AnimBP corps et visage, les 25 poses de visemes `MHF_*`, les 16 poses
> d'emotions et le suivi du regard. « Se defaire de Convai » n'a jamais
> voulu dire desinstaller ce plugin.
