# Les emotions et le lipsync se disputent le meme AnimBP

**Constat du 30/07/2026, a trancher.** Le maillage facial n'a qu'UN
AnimBP, et les deux systemes qui veulent l'animer n'habitent pas le meme.

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
