# Protocole — faster-whisper contre NVIDIA Riva (ACE ASR)

## La question

**Faut-il deplacer la reconnaissance vocale du sidecar vers Unreal ?**

Ce n'est pas une question de millisecondes. C'est une question de mode :

| | faster-whisper | Riva (ACE ASR) |
|---|---|---|
| Transcription | **par lot**, une fois le segment clos | **en flux**, pendant que la personne parle |
| Delai percu | tout le temps de transcription se paie APRES le silence | finalisation ~1200 ms apres le silence |
| Ou | sidecar Python, via WebSocket | dans Unreal, en processus |
| VAD | Silero, plugin separe | integre |
| Modele francais | `medium`, 769M | `conformer-fr-140m`, 140M |

Le gain possible : plus de reechantillonnage 48 -> 16 kHz, plus de trames
PCM sur la socket, un plugin de moins, et un delai qui commence AVANT que
le visiteur se taise.

Le risque : **un modele cinq fois plus petit**.

## Ce qui decide, et ce qui ne decide pas

**NE DECIDE PAS : la latence.** Le STT pese 820 a 1200 ms sur le poste de
developpement, mais c'est un artefact — Whisper y tourne sur processeur
faute de VRAM. Sur la borne en `cuda/float16`, l'etape retombe vers 300 ms
sur ~1,1 s de total. Gagner 200 ms ne justifie pas de changer d'architecture.

**DECIDE : la qualite sur les enonces COURTS en francais.**

Le projet a deja tranche une fois, dans l'autre sens : `whisper small` a ete
abandonne le 29/07/2026 parce qu'il rendait « S'attu. », « pandinvestigation »,
« Je le viens de Paris » — et l'agent repondait a du charabia sans que rien
ne signale l'anomalie. Un Conformer 140M est **plus petit encore que small**.
Rien ne dit qu'il echouera — l'architecture n'est pas la meme, et Riva est
entraine pour le temps reel — mais c'est la l'incertitude, et nulle part
ailleurs.

Or un visiteur repond « Oui », « Paris », « Ma soeur ». C'est presque tout
ce qu'il dit. Un moteur excellent sur les phrases longues et mediocre sur
deux mots est **inutile ici**.

## Seuils

| Mesure | Seuil de bascule |
|---|---|
| WER sur enonces courts (`court*`, `nom*`, `chiffre*`) | **<= celui de `medium/cuda`** |
| WER global | ecart tolere jusqu'a +0,05 |
| Transcriptions jugees inexploitables a la main | **aucune** sur les enonces courts |
| Latence fin de parole -> texte final | informatif seulement |

Si le WER court de Riva depasse celui de `medium`, **on ne bascule pas**,
quelle que soit la latence. Le cout d'une transcription fausse est un agent
qui repond a cote devant du public ; le cout de 200 ms est imperceptible.

## Le corpus : enregistre, jamais synthetise

Tentation a ecarter : passer du Piper dans les deux moteurs. C'est
reproductible, immediat, et **faux**. Une voix de synthese articule
parfaitement, ne bafouille pas, n'a pas de salle autour d'elle. Les deux
moteurs y reussiraient, et le cas qui les separe — un « ma soeur » lance
vite, de biais, dans un hall de musee — n'apparaitrait jamais.

Le corpus doit donc etre **enregistre sur la borne, avec son micro, dans
le lieu**, par plusieurs voix si possible (une voix grave, une voix aigue,
un accent non parisien).

25 enonces, definis dans `sidecar/bench/comparer_stt.py` :

- 8 tres courts (un a trois mots) — **le cas critique**
- 3 noms propres, 3 chiffres et dates — le point faible connu des petits modeles
- 5 reponses de visiteur ordinaires
- 4 adverses : hesitation, reprise, phrase inachevee
- 2 longs, pour contraste

## Deroule

### 1. Enregistrer — sur la borne

```
python bench/comparer_stt.py enregistrer
```

Ecrit `bench/corpus_stt/*.wav` a 16 kHz mono, plus `reference.json`. La
reference vit AVEC les enregistrements : un corpus retrouve six mois plus
tard sans elle ne se compare a rien.

### 2. Cote Whisper — automatise

```
python bench/comparer_stt.py whisper
```

Transcrit tout le corpus avec `base/cpu`, `small/cuda` et `medium/cuda`.
Les configurations indisponibles sur la machine sont signalees et sautees.

### 3. Cote Riva — DEMANDE UNE LIGNE DE PATCH

C'est le point dur. L'API Blueprint d'ACE ASR n'expose que la **capture
micro en direct** :

```
StartACEASRMicrophoneTranscription(DeviceIndex)
```

Or comparer deux moteurs sur deux enregistrements differents ne prouve
rien. Il faut le MEME audio dans les deux.

La methode existe deja dans le plugin, dans un en-tete **public** :

```cpp
// Plugins/ACE_ASR/Source/ACE_ASR/Public/ACEASRNVIGIRuntime.h:37
bool SubmitStreamingAudio(const TArray<int16>& PCM16Samples, FString& OutError);
```

mais `class FACEASRNVIGIRuntime` **n'est pas marquee `ACE_ASR_API`** : elle
n'est pas exportee du DLL, et l'appeler depuis le module GardeFrontiere
echoue a l'edition de liens.

**Patch a appliquer** (une ligne, meme pratique que
`patches/NV_ACE_Reference-UE5.7.md`) :

```cpp
class ACE_ASR_API FACEASRNVIGIRuntime      // etait : class FACEASRNVIGIRuntime
```

Puis, cote GardeFrontiere : une commande console `gf.RivaTranscrire <dossier>`
qui charge chaque WAV, le pousse dans `SubmitStreamingAudio`, note le texte
final et le temps ecoule, et ecrit `resultats_riva.json` au format attendu
par l'etape 4 :

```json
{ "court01": { "riva-fr-140m": { "texte": "oui", "ms": 210 } } }
```

> **Solution de repli**, si le patch est refuse : router les WAV vers
> l'entree micro par un peripherique audio virtuel, et utiliser la capture
> normale. Meme audio, mais un etage de plus dans la chaine — donc des
> latences non comparables. A n'employer que pour juger la QUALITE.

### 4. Rapport

```
python bench/comparer_stt.py rapport
```

Sort le WER par enonce et par moteur, le WER des enonces courts isole, et
la latence mediane.

## Puis relire a la main

Les chiffres ne suffisent pas, et le rapport le rappelle a l'ecran.
« Rion » et « lyon » pour « Lyon » ont un WER voisin et des consequences
opposees : l'un fait deriver l'entretien, l'autre ne se voit pas. Ouvrir
les JSON, lire les 25 transcriptions de chaque moteur, et juger : **est-ce
exploitable par un garde-frontiere ?**

C'est ce jugement-la qui tranche, le WER ne fait que le guider.

## Ce qui reste vrai quel que soit le resultat

Meme si Riva gagne, il faudra :

- verifier la **licence** *NVIDIA Models Community License* (livree dans
  l'archive) — dossier a instruire avec les deux autres en suspens ;
- garder faster-whisper en repli : il tourne sur processeur, donc sur
  n'importe quelle machine, ce que Riva ne fait pas (TensorRT-RTX) ;
- refaire passer `bench/dialogue_test.py`, dont les regles ne changent pas
  mais dont les entrees, elles, changeraient.
