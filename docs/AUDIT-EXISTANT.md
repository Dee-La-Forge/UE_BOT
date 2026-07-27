# Audit de l'existant — GardeFrontiereByDboy

Analyse du projet d'origine, realisee avant refonte.
Source : `../GardeFrontiereByDboy/`

## Nature

Unreal Engine **5.7**. Module C++ runtime unique, **vide** (boilerplate seul) :
toute la logique est en Blueprint. Carte unique `Studio.umap` (studio virtuel :
5 spots, 3 rect lights, 3 point lights, 5 PostProcessVolumes, 4 CineCameras,
demi-sphere HDRI).

Reglages moteur explicitement orientes borne fixe : `t.MaxFPS=60`, RayTracing
materiel coupe (Lumen software), MegaLights off, PSO precaching actif.
Commentaires d'origine : « audit stabilite borne », « anti-crash session
longue ».

## Empreinte disque : ~33 Go

| Dossier | Taille |
|---|---|
| Content | 10,2 Go |
| Saved | 6,8 Go |
| Intermediate | 4,6 Go |
| Plugins | 4,2 Go |
| Export + Windows (builds packages) | 5,2 Go |
| DerivedDataCache | 1,3 Go |

## Defauts releves

### Critiques

1. **Aucun versionnement.** Depot git present mais **zero commit**, pas de
   `.gitignore`, pas de LFS. 33 Go non suivis.
2. **God Blueprint.** `BP_ConvaiCharacterBase` cumule orchestration Convai,
   selection des MetaHumans, gating micro, post-process glitch, widget
   tampon, etat LiDAR, cycle de session, timers. Indiffable, intestable.
3. **Dependance cloud non securisee.** Coupure reseau = borne morte, sans
   mode degrade. Les logs montrent `gRPC channel not ready yet` et
   `Session ID : -1` repetes.
4. **Gestion d'erreur par Print String.** `BEGINPLAY CAST FAILED`,
   `ERREUR: CLEAN DU CHATBOT`, `Error PPV`. Aucune telemetrie, aucun
   watchdog.

### Structurels

5. **Tout est en dur** : port COM, baud rate, seuils, `AbsenceCount`,
   `SilenceDelay`, `VoiceThreshold`, Character IDs.
6. **~5 Go de contenu mort** : 5 MetaHumans importes, **3 references**
   (Germain, Louise, Trinity), **1 seul place** dans la map (Germain).
   Grondin et Smith sont du poids mort integral. Plus 18 maps de presets
   d'eclairage et 394 Mo de PoliceUniform.
7. **Builds packages dans le projet** (`Export/`, `Windows/` : 5,2 Go).
8. **Plugins vendorises** sans trace de version (4,2 Go).

### Hygiene

9. Residus de renommage : redirects `TP_Blank → /Script/refonte`, chemins
   fantomes (`/Game/Maps/StageEditor/...`, `/Game/Widget/WBP_Stamp`).
10. Config **Android active** dans une borne Windows, avec `SecurityToken`
    en clair.
11. `+CulturesToStage=en` alors que l'experience est en francais.

## Ce que Convai fournit reellement — 5 services

| # | Service | Point d'accroche Blueprint |
|---|---|---|
| 1 | STT | `OnTranscriptionReceived_V2` → `Transcription`, `IsTranscriptionReady` |
| 2 | LLM + logique d'entretien | `OnTextReceived_V2`, `OnNarrativeSectionReceived` → `NarrativeSectionID` |
| 3 | TTS | flux via `ConvaiAudioStreamer` → `OnStartedTalking` / `OnFinishedTalking` |
| 4 | Lipsync / blendshapes | `UConvaiFaceSyncComponent` |
| 5 | Memoire de session | `Session ID`, `Cleanup`, LTM |

Le **verdict** transite par `NarrativeSectionID` (on voit `SECTION 2`,
`Decision`, `AlreadyTriggered`, `IsExitStampPhase`), et `WBP_Stamp` fait un
simple `Contains` sur une chaine `Decision`.

## A CONSERVER — le plugin Convai n'est pas a jeter

Decision structurante : **« se separer de Convai » ne veut pas dire
« desinstaller le plugin »**. Toute la couche animation vit dedans.

`Plugins/Convai/Content/MetaHumans/Animations/` — 242 assets, 696 Mo :

| Chemin | Role | Verdict |
|---|---|---|
| `Convai_MetaHuman_BodyAnim` / `FaceAnim` | AnimBP principaux | ✅ garder |
| `AnimBP/` (6) | clignements, regard, tete, posture | ✅ garder |
| `Instruments/Enm` + `Functions` | `E_Emotions`, `E_Lips`, `E_InConversation` | ✅ garder |
| **`Motions/Lips/` (25)** | **viseme poses par phoneme** | ✅ **piece maitresse** |
| `Motions/Eyes/` (12), `HeadLook/` (10) | regard et tete directionnels | ✅ garder |
| `Motions/Idle/` (3), `Motions/Talk/` (4) | veille, gestuelle de parole | ✅ garder |
| `Motions2/Face/` (16) | emotions Angry/Concerned/Happy/Neutral/Stare ×3 | ✅ garder |
| `Motions2/*/Idle/` | variantes d'attente debout | ✅ garder |
| `Motions2/FP/Jog`, `Walk`, `Breaks` (~34) | locomotion joueur | ❌ inutile |
| `Motions2/FT|MT/Walk` (16) | idem | ❌ inutile |
| `Motions/Movement`, `Motions/Turn` (12) | marche, demi-tours | ❌ inutile |
| `SK_Preview/` (23) | maillages de previsualisation | ❌ editeur seul |

> ⚠️ **Ne pas elaguer a l'aveugle.** `B1D_MH_Movement` et `B2D_MH_HeadLook`
> sont des blend spaces qui referencent probablement une partie des anims de
> locomotion. Le tri se fait avec le Reference Viewer ouvert, **en fin de
> chantier**.

### Les 25 poses de visemes

```
MHF_AA  MHF_CH  MHF_EE  MHF_FV  MHF_II  MHF_KG  MHF_NL  MHF_None
MHF_OH  MHF_OU  MHF_PBM MHF_RR  MHF_SZ  MHF_TD  MHF_TH
        (+ variantes « n » : AAn CHn EEn IIn KGn NLn OHn PBMn SZn)
```

Indexees par **phoneme**. Utilisables comme **mode degrade** du lipsync si
NeuroSync est indisponible : un TTS qui expose ses phonemes alimente
directement ces poses, sans modele d'inference.

## Autres composants reutilisables

| Composant | Role |
|---|---|
| `UConvaiPlayerComponent` | capture micro, VAD, gain |
| `UConvaiAudioStreamer` | lecture audio en flux, dispatch visemes |
| `IConvaiLipSyncInterface` | **interface pluggable** — couture pour un lipsync maison |
| `BP_LidarManager` | lecture serie propre : `OpenComPortWithFlowControl` → `ReadString` → `Trim` → `StringToInt` → seuil → `AbsenceCount` |

## A remplacer

`ConvaiGRPC`, la couche reseau de `ConvaiChatbotComponent`, les proxies REST.
C'est la, et uniquement la, qu'est la latence.

## Ce qui n'est PAS dans le depot

La personnalite de l'agent, ses questions, ses regles de decision et le
graphe Narrative Design complet. Tout est sur les serveurs Convai, sous le
Character ID `080151a8-4885-11f1-a397-42010a7be02e`.

**A exporter avant toute resiliation d'abonnement.**
