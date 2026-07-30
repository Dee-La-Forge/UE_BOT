# Sidecar IA locale

Service IA local qui remplace quatre des cinq prestations que Convai rendait
par le reseau : STT, LLM, TTS et memoire de session. Le lipsync, lui, est
assure cote Unreal par **Audio2Face** (NVIDIA ACE, local) — voir
`docs/LIPSYNC-DECISION.md` : NeuroSync et le repli par visemes MHF_* sont
morts tous les deux.

## Pourquoi un processus separe

Le sidecar ne tourne pas dans Unreal, par choix d'architecture. Le loopback
localhost coute moins d'1 ms — negligeable — et on y gagne trois choses
decisives pour une borne en autonomie :

1. **si l'IA plante, Unreal survit** et bascule en mode degrade ;
2. le service IA redemarre seul, sans relancer l'application ;
3. on itere sur les prompts **sans recompiler le projet**.

Le LLM va meme un cran plus loin : il tourne dans son propre processus
(llama.cpp server), donc un crash du modele ne touche pas le sidecar.

## Le principe qui fait le gain

**Le gain de latence vient du streaming, pas du fait d'etre "local".**

```
LLM  ──token──token──token──[phrase 1 complete]──token──token──...
                                    │
                                    ▼
TTS                          synthese phrase 1
                                    │
                                    ▼
Audio                        LECTURE DEMARRE  ◄── pendant que le LLM ecrit
```

Fait naivement — attendre la reponse LLM complete, puis synthetiser tout —
le local serait **plus lent** que Convai malgre la disparition du reseau.

## Installation

```bash
./scripts/telecharger.sh   # voix Piper + LLM + llama.cpp (tailles verifiees)
```

```powershell
.\scripts\setup.ps1        # environnement Python (venv + pip)
```

Puis, dans deux terminaux :

```powershell
.\scripts\lancer_llm.ps1                              # terminal 1
.venv\Scripts\python.exe -m bench.bench_pipeline --tours 5   # terminal 2
```

## Mesurer la latence

C'est l'objet du prototype. Le bench chronometre chaque etage :

```
  Decomposition de la latence
  ------------------------------------------
  STT (transcription)           ... ms
  LLM (1er token)               ... ms
  LLM (1re phrase)              ... ms
  TTS (1er chunk)               ... ms
  ------------------------------------------
  PREMIER SON                   ... ms
```

Les releves s'accumulent dans `../benchmarks/latence.jsonl`.

**Reference a battre : ~2500 - 4000 ms** (estimation de la chaine Convai
actuelle). Cible : **600 - 1200 ms**. Atteint : **694 ms** jusqu'au premier
son (releves dans `../benchmarks/latence.jsonl`).

Comparer les deux LLM sur la meme machine :

```powershell
.\scripts\lancer_llm.ps1 -Modele qwen2.5-7b-instruct-q4_k_m.gguf
.\scripts\lancer_llm.ps1 -Modele qwen2.5-3b-instruct-q4_k_m.gguf
```

Le 3B genere 2 a 3× plus vite ; s'il tient le personnage, il gagne. **C'est
la mesure qui tranche, pas l'intuition.**

## Structure

```
main.py                  point d'entree — serveur WebSocket
config.yaml              tous les reglages (plus rien en dur)
grammars/
  entretien.gbnf         1 phrase, verdict INTERDIT   (< questions_min)
  verdict.gbnf           2 phrases, verdict autorise  (>= questions_min, 6)
  cloture.gbnf           2 phrases, verdict OBLIGATOIRE (>= questions_max, 10)
scenario/agent.yaml      scenario transcrit du Narrative Design
src/
  serveur.py             WebSocket — contrat d'evenements vers Unreal
  machine_etats.py       phases et bascule de grammaire — sans dep. lourde
  pipeline.py            orchestration entrelacee, bornes de temps par etage
  stt.py                 faster-whisper (GPU, repli CPU)
  llm.py                 client streaming vers llama.cpp server
  tts.py                 Piper (CPU)
  texte.py               nettoyage avant TTS
  metrics.py             chronometrage par etage
bench/
  bench_pipeline.py      chronometre           (demande les modeles)
  client_test.py         simule Unreal, session complete
  generer_echantillons.py
  test_machine_etats.py  17 verifications      (sans modele)
  test_decoupage.py       7 cas — le VRAI ClientLLM.phrases (sans modele)
  test_serveur.py         6 cas — sessions, verrous, connexions (sans modele)
  test_texte.py          14 cas                (sans modele)
scripts/setup.ps1, lancer_llm.ps1, telecharger.sh
```

## Lancer la borne

Trois processus, volontairement separes — si l'un tombe, les autres
survivent :

```powershell
.\scripts\lancer_llm.ps1 -Modele qwen2.5-3b-instruct-q4_k_m.gguf   # 1
.venv\Scripts\python.exe main.py                                    # 2
# 3 = Unreal, ou pour tester sans lui :
.venv\Scripts\python.exe -m bench.client_test
```

## Trois lecons de la mise au point

Elles ont chacune coute une regression, et valent d'etre retenues.

**1. Ne decrivez PAS le format de sortie dans le prompt quand une grammaire
le contraint deja.** Une version detaillait `[EMOTION:X][VERDICT:Y]` et la
liste des emotions dans le bloc systeme. Le modele s'est mis a recracher ce
vocabulaire en prose — *"Neutral Happy Concerned Angry X Verditt :Neutral"* —
faute de pouvoir ecrire les vrais crochets, que la grammaire lui interdit.
L'explication n'etait pas redondante : elle etait nuisible.

**2. Interdire un caractere dans la grammaire peut supprimer le jeton
d'arret.** Bannir `<` pour empecher les fuites de `<tool_call>` bloque du
meme coup `<|im_end|>` : le modele ne peut plus s'interrompre seul et
deroule tout le scenario en une replique, en jouant les deux roles. Il faut
alors borner explicitement la longueur — ce que fait `replique ::= phrase`.

**3. Rien de variable dans le bloc systeme.** Le compteur de questions y
figurait ; il invalidait le cache de prompt de llama.cpp a chaque tour.
Le sortir vers le message utilisateur a fait passer la latence de
**1064 ms a 611 ms**, sans rien changer d'autre.

## Le verdict, rendu deterministe

L'ancien systeme dependait du parsing des "narrative sections" renvoyees par
Convai. Ici, une **grammaire GBNF** force le modele a terminer par :

```
Bonjour. Presentez-moi vos papiers.[EMOTION:Concerned][VERDICT:EN_COURS]
```

Le modele ne *peut pas* produire autre chose. `EMOTION` s'aligne sur l'enum
`E_Emotions` du plugin Convai (`Motions2/Face/`), `VERDICT` declenche le
tampon `WBP_Stamp`. Les tags arrivent **apres** le texte parle, pour que la
lecture demarre sans les attendre.

## Etat de ce prototype

Le sidecar **tourne en conditions reelles** depuis le 27/07/2026 : sessions
completes LiDAR → STT → LLM → TTS → verdict, 694 ms jusqu'au premier son,
releves accumules dans `../benchmarks/latence.jsonl`. Les sessions des
27-29/07 ont produit les correctifs consignes dans les commentaires de
config.yaml et des sources.

Ce qui reste a valider : la tenue sur la duree (test thermique 2 h) et le
comportement avec le micro definitif de la borne.

`scenario/agent.yaml` porte la persona d'origine transcrite, mais la
**backstory / knowledge bank** du personnage Convai (Character ID
`080151a8-4885-11f1-a397-42010a7be02e`) reste a exporter **avant toute
resiliation d'abonnement**.
