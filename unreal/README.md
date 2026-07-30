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
   │           Accueil (l'agent en poste interpelle)    │
   │               │                                    │
   │               ▼                                    │
   │       Interrogatoire (6 a 10 questions)            │
   │               │                                    │
   │               ▼                                    │
   │           Verdict ──► [TAMPON accepte | refuse]    │
   │               │                                    │
   │               ▼                                    │
   │        SortieZone ──► [PANNEAU "quittez la zone"]  │
   │               │                                    │
   └── capteur : absence ──► [GLITCH + SWITCH AVATAR]  ─┘
                   ▲          (zone vide uniquement —
                   │           sinon substitution differee)
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
| Sidecar injoignable | replique de secours (**une annonce par session**), reconnexion toutes les 5 s |
| Sidecar gele, socket ouverte | watchdog `DelaiReponseSidecar` : deconnexion forcee, meme chemin |
| Capteur debranche | session en cours liberee, reouverture du port en boucle |
| Aucune interaction | abandon apres `DelaiAbandon` |
| Zone encore occupee apres un abandon | nouvelle session apres `DelaiReprisePresence` (le capteur ne rend que des fronts) |

Le micro est par ailleurs **sourd pendant que l'agent est audible** (flux,
tampon ACE, marge `MargeEchoApresReplique`) : sans ce garde, la voix TTS
captee par le micro repartait au sidecar comme parole du visiteur.

## AVANT LA MISE EN SERVICE — reglages de diagnostic a annuler

Ils rendent la mise au point possible sur un poste sans LiDAR, sans micro
et sans RTX. Sur la borne, ils doivent tous repasser a leur valeur reelle.

| Ou | Reglage | Poste de dev | Borne |
|---|---|---|---|
| `unreal/Config/DefaultEngine.ini` | `gf.SessionAuto` | `3` (ouvre une session sans capteur) | **`0`** |
| `sidecar/config.yaml` | `stt.peripherique` / `type_calcul` | `cpu` / `int8` (la 1060 n'a pas la VRAM) | **`cuda` / `float16`** |
| `GuardSessionManager` | `bAfficherEtatEcran` | coche | **decoche** |
| `LidarPresenceComponent` | `bTracerReleves` | coche | **decoche** |
| `GuardSessionManager` | `bActiverCapteur` | sans effet ici (pas de COM4) | **coche** |

## Legacy Convai — RETIRE le 31/07/2026

Le plugin reste installe pour ses seules animations MetaHuman. Son cablage
cloud, lui, survivait dans des assets binaires que le C++ ne peut pas
nettoyer. Trois symptomes etaient listes ici ; ils n'avaient que **deux**
causes, et c'est le diagnostic par script qui l'a etabli.

| Symptome | Cause reelle | Etat |
|---|---|---|
| `LogLoad: Game class is 'ConvaiDemoGM_C'` puis `Empty API Key` | **GameMode Override** de `Studio.umap` | vide — le journal dit desormais `Game class is 'GardeFrontiereGameMode'` |
| `ConvaiPlayerLog: Found submix "AudioInput"` | **le meme override** : `ConvaiDemoGM` instancie son propre pion, qui porte le `ConvaiPlayerComponent` | disparu avec lui |
| « BEGINPLAY CAST FAILED » **affiche a l'ecran** | Event Graph de `BP_ConvaiCharacterBase` | vide |

> Ce README affirmait qu'« un acteur de la carte porte encore un
> `ConvaiPlayerComponent` ». C'etait faux : le diagnostic Python a compte
> **zero** composant Convai sur les 15 acteurs. Le composant naissait du
> GameMode. Chercher un acteur inexistant aurait pu couter longtemps —
> l'override, lui, se voyait en une ligne de propriete.

L'override a ete **efface**, pas remplace : `DefaultEngine.ini` porte deja
`GlobalDefaultGameMode`, et deux sources de verite pour une meme valeur
finissent toujours par diverger.

Reste au journal `ConvaiSubsystemLog: gRPC Creating Channel` : c'est le
sous-systeme du plugin qui demarre, inherent au fait de le garder installe.
Sans cle API et sans composant conversationnel, il ne joint personne. Le
faire taire voudrait dire desinstaller le plugin, donc perdre les
animations MetaHuman — voir « Assets migres ».

### Corriger un asset binaire par script

`PythonScriptPlugin` est active dans le `.uproject` (plugin EDITEUR : rien
n'en part dans un build package). Il rend verifiable ce qui ne l'etait
pas — un asset binaire peut etre inspecte et corrige de facon
reproductible, au lieu d'etre decrit de memoire.

```
UnrealEditor-Cmd.exe unreal\GardeFrontiere.uproject ^
    -run=pythonscript -script="chemin\script.py" -unattended -nosplash
```

Deux choses a savoir :

- **Charger la carte d'abord.** En commandlet, aucun monde n'est ouvert :
  `LevelEditorSubsystem.load_level("/Game/Studio")`.
- **Le code de sortie 255 n'est pas un echec.** Unreal tente d'« extraire »
  le fichier du controle de revision, n'y arrive pas, ouvre une boite de
  dialogue — et sauvegarde quand meme juste apres. Verifier le fichier sur
  disque, pas le code de retour.

Ce qui reste hors de portee du script : l'edition d'un GRAPHE Blueprint.
L'API Python sait compiler, reparenter, retirer des variables inutilisees ;
elle n'expose pas la suppression de noeuds.

### Le God Blueprint ne se supprime pas — il se vide, et dans cet ordre

Constat verifie le 31/07/2026 en lisant les tags d'AssetRegistry des
`.uasset` (le comptage de chaines ne suffit pas : il ne distingue pas un
heritage d'un simple *Cast*) :

```
Character  ──►  BP_ConvaiCharacterBase  ──►  BP_AgentGermain
(natif)         ParentClass = Character      ParentClass = ...Base_C
                1 composant propre :         19 composants :
                DefaultSceneRoot             Body, Face, Hair, coat...
```

Le parent est donc **de la logique pure** : toute la scenographie du
MetaHuman vit chez l'enfant. Il n'y a **rien a supprimer dans sa liste de
composants** — le `ConvaiChatbotComponent` y est une *variable* interrogee
par l'ubergraph, pas un noeud de construction ; elle s'en va avec l'Event
Graph. Le `Capsule` et l'`Arrow` sont hereditaires et non supprimables.

**Tout le nettoyage est dans le parent.** Verifie le 31/07/2026 en relevant
les noeuds des deux graphes :

| | `BP_AgentGermain` | `BP_ConvaiCharacterBase` |
|---|---|---|
| `ReceiveBeginPlay` | **0** | 3 |
| chaine « BEGINPLAY CAST FAILED » | **0** | 2 |
| composant Convai | `ConvaiChatbot_REMOVED_...` | (variable seule) |

`BP_AgentGermain` est **deja propre** : son composant Convai porte le suffixe
`_REMOVED_` qu'Unreal donne aux objets supprimes, et son graphe ne contient
plus que de la logique MetaHuman — un `Event Tick`, les deux
`OnAnimInitialized` du corps et du visage, et des `Cast` vers
`Convai_MetaHuman_BodyAnim_C` / `FaceAnim_C`. **Ces Cast-la se gardent** :
ce sont les AnimBP du plugin, que le projet conserve volontairement (voir
« Assets migres »). Les supprimer arreterait les animations.

Reste donc un seul geste, dans le parent : **vider son Event Graph**. Ce
qu'il contient encore se lit dans ses propres etiquettes de debogage, et
c'est mot pour mot l'inventaire repris en C++ — `INITIALISATION DE LA
SCENE`, `SWITCH PERSONNAGE`, `MICRO ON` / `MICRO OFF`, `GLITCH MANAGER`,
`CLEAN DU CHATBOT`, `RELOAD APRES ABANDON`, `RESET PERSONNAGE`,
`CLEAR CHAT`, `ACTIVE MH SET`.

Ses **variables** se traitent apres, et seulement une fois le graphe vide :
tant qu'un noeud les lit, les retirer casse la compilation.

> Tant que cette chaine existe, `RetirerConvaiConversationnel` (dans
> `AvatarSwitcherComponent.cpp`) est sur le **chemin nominal**, pas en
> secours : tout avatar spawne herite du cablage Convai. Le reparentage de
> `BP_AgentGermain` sur `Character` — annonce dans un commentaire, jamais
> fait — reste la vraie sortie, mais il se fera a tete reposee.

## Reste a faire

- [x] ~~Trancher : emotions ou lipsync ?~~ **Les deux** : l'emotion du LLM
      voyage avec l'audio vers Audio2Face, qui l'applique en meme temps
      que le lipsync (`EmotionPourA2F`). Reste a **regler les quatre
      correspondances a l'oeil, sur la borne** — voir
      `docs/EMOTIONS-VS-LIPSYNC.md`
- [x] ~~Supprimer le noeud `Set VADProvider` de `BP_FirstPersonCharacter`~~ :
      ancien cablage, il empeche le Blueprint de compiler (5 erreurs) pour
      une fonction que `UVisitorMicComponent` assure deja en C++ — voir
      `patches/RuntimeAudioImporter-VAD.md`
- [x] Migrer les assets depuis l'ancien projet (3,4 Go, verifie par comptage)
- [x] Blueprint de scenographie branche sur les evenements
- [x] Capture micro + VAD vers `EnvoyerAudioVisiteur`
- [x] Lecture des trames audio recues (Audio2Face + repli `Voix`)
- [ ] Micro physique sur la borne (aucun peripherique de capture)
- [ ] Test thermique 2 h, pile complete

> Le lipsync ne passe PAS par LiveLink ARKit ni par les visemes MHF_* :
> ces deux chemins sont morts (voir `docs/LIPSYNC-DECISION.md`). C'est
> **Audio2Face** (NV ACE, plugins locaux + patch UE 5.7, voir `patches/`)
> qui anime le visage a partir des trames audio du sidecar.
>
> ⚠️ **Audio2Face exige une carte RTX** (coeurs Tensor). Sur un poste
> Pascal — GTX 1060 essaye le 30/07/2026 — le plugin se charge mais
> l'inference refuse : « No supported hardware found ». La borne bascule
> alors en mode degrade (l'agent parle, bouche fermee), ce qui reste un
> chemin utile a tester. **Le lipsync ne se valide que sur la borne.**
> Detail dans `patches/NV_ACE_Reference-UE5.7.md`.

### Assets migres

> ⚠️ **`Content/MetaHumans/Common/Face/Face_AnimBP` ne vient d'aucun des
> deux projets** : il est extrait du projet d'exemple Kairos de NVIDIA et
> depose a la main. Sans lui, le visage ne bouge plus du tout — ni par
> Audio2Face, ni par les emotions. Voir `patches/NV_ACE_Reference-UE5.7.md`.

| Depuis l'ancien projet | Poids |
|---|---|
| `Content/MetaHumans/AgentGermain` | 721 Mo |
| `Content/MetaHumans/Common` | 253 Mo |
| `Plugins/Convai/Content/MetaHumans/Animations` | 696 Mo |
| `Content/PoliceUniform` | 394 Mo — d'abord exclu, puis migre : l'agent apparaissait nu sans lui |
| `Content/GlitchFx`, `StampFx`, `Materials`, `Meshes` | 9 Mo |

**Non migres** : `AgentGrondin` (737 Mo) et `AgentSmith` (707 Mo), sans
Character ID ni reference ; les 18 maps de presets d'eclairage MetaHuman.

> Le dossier `Animations` du plugin Convai est **indispensable** : il porte
> les AnimBP corps et visage et les 16 poses d'emotions. « Se defaire de
> Convai » n'a jamais voulu dire desinstaller ce plugin — mais le droit
> d'utiliser ces assets apres resiliation de l'abonnement reste a verifier
> (voir THIRD-PARTY.md a la racine).
