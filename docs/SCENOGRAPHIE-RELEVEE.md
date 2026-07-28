# Scenographie — relevee dans BP_ConvaiCharacterBase

Lecture directe des graphes Blueprint, 28/07/2026. Sert de specification a
la reimplementation C++ : ces valeurs sont **mesurees, pas supposees**.

## Structure du Blueprint

```
BP_ConvaiCharacterBase  (parent : Personnage)
├── Capsule Component (CollisionCylinder)
│   ├── Arrow Component
│   └── Mesh (CharacterMesh0)
│       └── ConvaiChatbot
└── Character Movement (CharMoveComp)

Fonctions : Construction Script · F_Update Environment · LoseFocusDelayed
            SwitchToExitStamp · HideAndSwitch
Variables : Story Begined · GlitchLoopActive · Is Detected · Is Glitching
            Is Exit Stamp Phase · Already Triggered · Active MetaHuman
            Active Convai Chatbot · Active Character · Stamp Widget
            PPV Glitch · Dynamic Glitch Mat · Metahuman Classes
```

## Initialisation de la scene

```
On Component Activated (CharacterMovement)
→ Get All Actors Of Class (PostProcessVolume) → GET[0]
→ Is Valid ─true→ SET PPV Glitch → Print "Init PPV OK"
│                → Create Dynamic Material Instance (Parent : PPM_GLITCH)
│                → SET Dynamic Glitch Mat
│                → Add or Update Blendable (In Weight 0.0)
└─false→ Print "Error PPV"
```

> ⚠️ Le volume est le **premier** renvoye par `Get All Actors Of Class`, pas
> un volume designe. Il y en a cinq dans `Studio.umap` et l'ordre n'est pas
> garanti. **A rendre explicite en C++** — une reference directe, editable.

Meme motif pour le capteur : `Get All Actors Of Class(BP_LidarManager)` →
`GET[0]` → `Bind Event to On Visitor Left`.

## Glitch — mecanisme complet

```
Glitch_FX     (evenement) → SET Glitch Loop Active = false
GlitchFXLoop  (evenement) → SET Glitch Loop Active = true
   └→ Branch(Is Glitching) → SET Is Glitching = true → Delay 0.2
      → Is Valid(PPV Glitch, Dynamic Glitch Mat)
      → Add or Update Blendable (In Weight 1.0)      // active l'effet
      → Play Sound 2D (Glitch_Sound_By_DyBoy)
      → GlitchTimeline (Play)

GlitchTimeline · Update
   → Set Scalar Parameter Value
        Target         : Dynamic Glitch Mat
        Parameter Name : "weight"
        Value          ← sortie Weight de la timeline

StopGlitch → SET Glitch Loop Active = false → SET Is Glitching = false
   → Sequence
        Then 0 : Is Valid → Set Scalar Parameter Value ("weight", 0.0)
        Then 1 : Add or Update Blendable (In Weight 0.0)
```

**Deux leviers distincts, a ne pas confondre :**

| | Role |
|---|---|
| `Add or Update Blendable` — In Weight | branche/debranche le materiau sur le PPV |
| `Set Scalar Parameter Value` — "weight" | anime l'intensite, pilote par la timeline |

Le premier est binaire (1.0 / 0.0), le second est continu. Couper l'un sans
l'autre laisserait soit un effet fige, soit un materiau inutilement actif.

## Tampon

```
Create WBP Stamp Widget (Class : WBP_Stamp, Owning Player)
→ SET Stamp Widget → Add to Viewport → Set Render Opacity 0.0
```

Le widget est cree **une seule fois** et reste en viewport ; on l'affiche et
le masque par l'opacite. Interface exposee : evenements `ShowStamp(Decision)`
et `ShowExitStamp`, variables `StampImage`, `Duration`, `Decision`.

## Switch d'avatar — SwitchPersonScene

```
SET Is Exit Stamp Phase
→ Is Valid(Stamp Widget) → Remove from Parent
→ Cast To BP_ConvaiCharacterBase
   → PPV Glitch + Dynamic Glitch Mat
   → Add or Update Blendable (In Weight 0.0)        // coupe le glitch
→ Destroy Actor (Active MetaHuman)                   // DETRUIT l'ancien
→ SpawnActor
     Class     ← GET[ Random Integer(max = LENGTH(Metahuman Classes)) ]
     Transform ← Location (0,0,0) · Rotation (0,0,90) · Scale (1,1,1)
     Collision : Default
→ Cast To BP_ConvaiCharacterBase
→ Get Player Pawn → Cast To BP_FirstPersonCharacter → SET Active Character
→ Get Component by Class (ConvaiChatbot)  → SET Active Chatbot
→ Get Component by Class (AudioCapture)   → Set Active (false, Reset : true)
→ SET Active MetaHuman
→ Delay 0.3
→ SET Already Triggered → SET Story Begined
→ SET Is Detected
→ Get Actor Of Class (BP_LidarManager) → Reset Lidar State
→ Init Lidar Binding → Clear Chatbot → Delay 0.2
→ Is Valid → F_Update Environment → Stop Glitch
```

**Le switch detruit et respawne** — ce n'est pas de l'activation/masquage.
Le tirage est purement aleatoire, sans garde contre la repetition : avec
trois avatars, le meme visage revient une fois sur trois.
(Ma version C++ evite la repetition immediate — voir `TirerAvatar`.)

## Sortie de zone

```
Set Timer by Function Name : SwitchToExitStamp — Time = 7.0
```

**7 secondes** apres le verdict avant d'inviter le visiteur a partir.
J'avais mis 4 s par defaut dans `DelaiAvantSortie` : corrige.

`OnVisitorGone` → `Branch(Is Exit Stamp Phase)`
- vrai  : `Clear Timer(HideAndSwitch)` → opacite 0 → `SET Is Exit Stamp Phase`
          → `SET Is Detected` → `Switch Person Scene`
- faux  : cascade `Is Detected` / `Story Begined` → `Reset Character`,
          `Glitch FX Loop`

## Reset apres abandon

```
ResetCharacter → Branch(Already Triggered) → Delay 2.0
→ Clear Chatbot → Is Valid → Stop Glitch
→ SET Already Triggered → Switch Person Scene
   Print "RELOAD APRES ABANDON"
```

## Fin de session

```
CE_EndSession → Stop Glitch → SET Voice Active
→ Get Component by Class(ConvaiPlayer) → Finish Talking
→ Clear Chatbot → SET Active Convai Chatbot → SET Active Character
→ Clear Timer(LoseFocusDelayed)
→ Get Anim Instance(Mesh) → Cast To Convai_MetaHuman_BodyAnim
   → SET Body Talk = 0.0
   → SET Random Talk = 0
```

## Micro

```
On Started Talking (ConvaiChatbot)
→ Get Player Pawn → Cast To BP_FirstPersonCharacter
→ Get Component by Class (AudioCapture) → Set Active
   Print "MICRO OFF"
```

Le micro est **coupe pendant que l'agent parle** — evite qu'il s'entende
lui-meme. A reproduire : c'est une precaution acoustique, pas un detail.

## ⚠️ Emotion : introuvable

Aucun noeud d'emotion dans les graphes parcourus. Ni `E_Emotions`, ni
ecriture sur l'AnimBP facial.

**Hypothese** : l'emotion est entierement interne au plugin Convai —
l'AnimBP la lit depuis le composant `ConvaiChatbot`, alimente par la reponse
du cloud. Le Blueprint n'a rien a cabler parce que le plugin s'en charge.

**Consequence, si l'hypothese tient** : avec le sidecar local, ce signal
disparait. Aucune emotion n'atteindra le visage, quoi que produise le tag
`[EMOTION:...]` du LLM.

Cela ferait de **LiveLink non plus un raffinement mais le seul chemin** vers
un visage expressif. Voir `LIPSYNC-DECISION.md`.

**A confirmer** : recherche `emotion` dans le Blueprint.
