# Les emotions et le lipsync se disputent le meme AnimBP

> ## RESOLU le 30/07/2026 — par une quatrieme voie
>
> Les trois options ci-dessous supposaient toutes qu'il fallait choisir
> entre l'AnimBP de Convai et celui de NVIDIA. C'etait une fausse
> alternative : **Audio2Face accepte une emotion imposee par
> l'application**, et l'applique au visage en meme temps qu'il calcule
> le lipsync.
>
> ```cpp
> SendAudioSamples(Echantillons, false, EmotionPourA2F(), nullptr);
> //                                    ^ TOptional<FAudio2FaceEmotion>
> ```
>
> Ce parametre etait passe a `NullOpt` depuis le montage de juillet :
> voila pourquoi l'agent parlait sans expression. `FAudio2FaceEmotion`
> porte `bEnableEmotionOverride` (vrai par defaut) et dix emotions
> surchargeables — Amazement, Anger, Cheekiness, Disgust, Fear, Grief,
> Joy, OutOfBreath, Pain, Sadness.
>
> `AGuardSessionManager::EmotionPourA2F` traduit desormais l'emotion
> decidee par le LLM vers ces surcharges. **Aucun Blueprint a editer, le
> LLM garde la main sur l'expression, et le lipsync est intact.**
>
> Et **Audio2Emotion est volontairement ecarte** : il DEDUIT l'emotion de
> l'audio, or Piper produit une voix plate — il detecterait « neutre » a
> chaque replique et la decision narrative serait perdue.
>
> Reste a valider sur la borne : le rendu des quatre correspondances
> (Stare / Concerned / Angry / Happy) ne peut pas se voir sans RTX.
> Les valeurs sont dans `EmotionPourA2F`, faciles a regler a l'oeil.

Le constat d'origine, conserve pour la trace :

---

Le maillage facial n'a qu'UN AnimBP, et les deux systemes qui veulent
l'animer n'habitent pas le meme.

## Le fait

`UAgentFaceComponent::AppliquerMelange` ecrit sept variables flottantes
sur l'AnimInstance du visage :

```
Anger, Joy, Sadness, Neutral, Afraid, Suprise, Bored
```

Ce sont celles de **Convai_MetaHuman_FaceAnim**. Or
`UAvatarSwitcherComponent::PreparerAudio2Face` impose, au spawn de chaque
avatar, l'AnimBP de NVIDIA — **Face_AnimBP** — qui porte le noeud
`ApplyACEAnimation` et ne connait aucune de ces sept variables.

Releve au journal, sur le meme poste, a une heure d'intervalle :

| AnimBP en place | Emotions ecrites | Lipsync Audio2Face |
|---|---|---|
| `Convai_MetaHuman_FaceAnim` (Face_AnimBP absent) | **5 sur 7** — `Neutral` et `Bored` manquaient deja | impossible : erreur au spawn |
| `Face_AnimBP` (installe depuis Kairos) | **0 sur 7** | possible (sur RTX — voir `patches/NV_ACE_Reference-UE5.7.md`) |

## Ce que cela implique, et depuis quand

La borne impose `Face_AnimBP` **depuis le montage Audio2Face de juillet
2026**. Les emotions y sont donc muettes depuis ce jour-la, et toute la
chaine en amont continue de tourner pour rien :

- la grammaire GBNF **force** le tag `[EMOTION:...]` a chaque replique ;
- le sidecar l'extrait et emet l'evenement `emotion` ;
- `SurEmotion` le recoit, `AgentFaceComponent` calcule son melange ;
- et l'ecriture echoue, sur les sept variables.

Le code le signalait — « aucune propriete d'emotion trouvee sur
l'AnimInstance » — mais l'avertissement ne sort qu'UNE fois par session
et personne ne l'avait relie a la bascule d'AnimBP.

Seule consequence visible aujourd'hui : le visage garde une expression
fixe. `OnEmotionChangee` continue d'etre diffuse vers le Blueprint, donc
une scenographie qui s'y branche fonctionne toujours.

## De quoi est faite l'expression, cote Convai

Inspecte le 30/07/2026 — tout est deja sur la machine, dans le plugin :

```
Convai/Content/MetaHumans/Emotions/Full_Emotion_spectrum.uasset          11,5 Mo
Convai/Content/MetaHumans/Emotions/Full_Emotion_NoMouth_spectrum.uasset  11,3 Mo
Convai/Content/MetaHumans/Animations/Convai_MetaHuman_FaceAnim.uasset     1,8 Mo
```

Les deux premiers portent les POSES ; l'AnimBP les melange a partir des
sept variables flottantes. La variante `NoMouth` existe precisement pour
cohabiter avec un lipsync qui pilote la bouche — c'est celle qu'il faut
pour l'option 1, sinon les emotions et Audio2Face se disputeraient les
levres.

**Recette de l'option 1** : ouvrir `Convai_MetaHuman_FaceAnim` pour voir
comment il branche `Full_Emotion_spectrum` sur les sept flottants, puis
reproduire ce montage dans `Face_AnimBP` a cote du noeud
`ApplyACEAnimation`, en prenant la variante `NoMouth`.

### Un piege de version a connaitre

Le plugin Convai existe en deux versions sur cette machine :

| Emplacement | Version | `FaceAnim` |
|---|---|---|
| Moteur (`Engine/Plugins/Marketplace/Convai`) | **3.3.2** (08/2025) | 1867 Ko |
| Build « validee par Didier » (archive) | **3.6.9** (03/2026) | 1873 Ko |

Le reste du contenu est identique au fichier pres (242 animations,
2 emotions, memes tailles) : seul l'AnimBP differe. `AgentFaceComponent`
ecrit sept variables et le journal n'en signale que deux absentes avec la
version du moteur — `Neutral` et `Bored`. Elles vivent donc tres
probablement dans la 3.6.9, contre laquelle le code a ete ecrit.

Sans consequence tant qu'on reste sur `Face_AnimBP` (aucune des sept n'y
existe). Mais quiconque revient a l'AnimBP de Convai doit savoir qu'il
lui faut la **3.6.9**, pas celle du moteur.

## Les trois issues

1. **Ajouter les sept variables a Face_AnimBP** (travail Blueprint : les
   declarer, puis brancher leur melange sur la pose, comme le faisait
   Convai). On garde le lipsync ACE **et** les emotions pilotees par le
   LLM. C'est la seule option qui preserve les deux, et elle valide tout
   ce qui a ete construit autour des tags.
2. **Assumer qu'ACE pilote tout**, emotions comprises (il embarque
   Audio2Emotion, qui les deduit de l'audio). Alors `AgentFaceComponent`
   devient du code mort a retirer, et les tags `[EMOTION:]` ne servent
   plus qu'a la scenographie Blueprint. Decision defendable, mais elle
   retire au LLM la maitrise de l'expression.
3. Revenir a `Convai_MetaHuman_FaceAnim` : emotions oui, lipsync jamais.
   A ecarter — c'est le chemin qu'on a quitte volontairement.

**Recommandation : l'option 1.** Le cout est une demi-heure d'edition
Blueprint ; le gain est un agent dont le visage suit ce qu'il dit.

## Detail a ne pas corriger

`Suprise` s'ecrit ainsi, sans le second `r`, dans l'AnimBP de Convai.
Corriger la faute cote C++ ferait echouer l'ecriture en silence. Si les
variables sont recreees dans Face_AnimBP (option 1), c'est l'occasion de
retablir l'orthographe — **des deux cotes en meme temps**.
