# ACE ASR (NVIDIA Riva) — installation et etat

**Statut : EN EVALUATION, PAS EN SERVICE.** Le STT nominal reste
faster-whisper, dans le sidecar. Rien n'appelle `InitializeASRAsync` :
le module se charge, precharge ses DLL ONNX (~50 Mo de VRAM mesures) et
ne fait rien de plus.

`unreal/Plugins/` n'est **jamais versionne** (voir `.gitignore`). Cette
page est donc la seule trace de l'installation : sans elle, elle se perd
au prochain clone.

## Pourquoi l'evaluer

Le STT est la moitie du delai mesure sur le poste de developpement :
820 a 1200 ms, contre ~580 ms pour le LLM et moins de 200 ms pour la voix.
Attention toutefois — c'est en grande partie un artefact de ce poste, ou
Whisper tourne sur PROCESSEUR faute de VRAM (voir `sidecar/config.yaml`).
Sur la borne en `cuda/float16`, cette etape retombe vers 300 ms.

L'interet de Riva n'est donc **pas la vitesse brute**, c'est le mode :

| | Whisper (faster-whisper) | Riva (ACE ASR) |
|---|---|---|
| Transcription | **par lot**, une fois le segment clos | **en flux**, pendant que la personne parle |
| Delai percu | tout le temps de transcription est APRES le silence | finalisation ~1200 ms apres le silence |
| Ou | sidecar Python, via WebSocket | dans Unreal, en processus |
| VAD | Silero, plugin separe | integre (`Silence Detection Time Ms`) |

En prime, le STT dans Unreal supprimerait le reechantillonnage 48 -> 16 kHz
et les trames PCM sur la socket.

## Ce qui reste a trancher, et qui ne se tranche que SUR LA BORNE

1. **La qualite sur des enonces courts en francais.** C'est LE critere.
   Le modele francais est un Conformer **140M** — et le projet a deja
   abandonne `whisper small` pour cette raison exacte : il massacrait les
   enonces courts (« S'attu. », « pandinvestigation ») et l'agent
   repondait a du charabia. Rien ne garantit qu'un 140M fasse mieux que
   `medium`.
2. **La latence reelle.** Backend GPU = TensorRT-RTX, donc RTX Turing ou
   plus recent. Une GTX 1060 retombe en DirectML ou CPU et ne mesure rien
   de representatif. Meme mur qu'Audio2Face.
3. **La licence** : *NVIDIA Models Community License*, livree dans
   l'archive. A verser au dossier avec les deux autres en suspens.

## Installation (refaite le 31/07/2026)

Source : `ACE_ASR_v5_7_1.0.4.zip`, fourni par le proprietaire du projet.
Le plugin declare `EngineVersion: 5.7.0` — **support natif**, aucun patch,
contrairement a `NV_ACE_Reference` (voir `NV_ACE_Reference-UE5.7.md`).

1. Extraire l'archive dans `unreal/Plugins/`, renommer le dossier racine
   en `ACE_ASR` (il sort en `ACE_ASR_v5_7_1.2`). ~0,96 Go.

2. Installer le modele francais. **Editeur ferme** : un processus en cours
   verrouille `nvigi.plugin.asr.riva-ort.gpu.dll`.

   ```powershell
   $d = "unreal\Plugins\ACE_ASR\ThirdParty\NVIGI\models\nvigi.plugin.asr.riva-ort\{7B9ED4D0-D15C-450F-BCC8-8FA7B460BFC0}"
   New-Item -ItemType Directory -Force -Path $d | Out-Null
   curl.exe -L "https://developer.nvidia.com/downloads/ace/speech-model/riva-ctc-140m-FR.zip" -o "$d\out.zip"
   tar.exe -x -f "$d\out.zip" -C $d
   Remove-Item "$d\out.zip"
   ```

   579 Mo telecharges, ~715 Mo installes : `fr-FR.bin`, `acoustic_mdl.onnx`
   (393 Mo), `punc_mdl.onnx` (168 Mo, ponctuation), `lexicon.txt`.

   > Le `download.bat` de NVIDIA fait exactement ces trois etapes. Le
   > lancer via `cmd /c "cd /d ... && download.bat"` echoue ici : le chemin
   > contient une espace ET des accolades. Les commandes ci-dessus
   > l'evitent.

3. Activer `ACE_ASR` dans `GardeFrontiere.uproject`, puis recompiler
   (le plugin a des sources : 8 `.cpp`).

4. **Pointer sur le francais**, sinon le plugin utilise le modele ANGLAIS
   livre dans l'archive. Deja fait dans `Config/DefaultGame.ini` :

   ```ini
   [/Script/ACE_ASR.ACEASRSettings]
   DefaultModelPreset=French_Conformer_140M
   ```

   Sans cette ligne, une premiere evaluation transcrirait du francais avec
   un modele anglais et conclurait — a tort — que Riva ne vaut rien.

## 5. PATCH A REAPPLIQUER — une ligne

**Sans lui, les deux moteurs ne sont pas comparables.** L'API Blueprint du
plugin n'expose que la capture micro EN DIRECT ; impossible d'y faire
entrer un fichier, donc impossible de faire passer le MEME audio dans Riva
et dans faster-whisper. Comparer deux moteurs sur deux enregistrements
differents ne prouve rien.

La methode existe pourtant, dans un en-tete **public** — il lui manque
seulement d'etre exportee du DLL.

Fichier : `Source/ACE_ASR/Public/ACEASRNVIGIRuntime.h`, ligne 21.

```cpp
class ACE_ASR_API FACEASRNVIGIRuntime     // etait : class FACEASRNVIGIRuntime
```

C'est tout. Les chemins d'inclusion NVIGI sont deja declares en
`PublicSystemIncludePaths` dans `ACE_ASR.Build.cs` : aucun autre reglage
n'est necessaire pour qu'un module tiers inclue cet en-tete.

**Verifier que le patch a pris** — recompiler ne suffit pas, le fichier
compilait deja avant. Il faut constater l'export :

```powershell
$d = "...\VC\Tools\MSVC\<version>\bin\Hostx64\x64\dumpbin.exe"
& $d /EXPORTS "unreal\Plugins\ACE_ASR\Binaries\Win64\UnrealEditor-ACE_ASR.dll" |
    Select-String SubmitStreamingAudio
```

Doit rendre une ligne. Releve le 31/07/2026 : `SubmitStreamingAudio`
exporte, avec le reste de `FACEASRNVIGIRuntime`.

> Cote GardeFrontiere, la suite reste a ecrire : ajouter `ACE_ASR` aux
> dependances de `GardeFrontiere.Build.cs`, puis une commande console
> `gf.RivaTranscrire <dossier>`. **Ce n'est pas anodin** : ce serait une
> dependance DURE de plus vers un plugin NVIDIA, et le Build.cs porte deja
> la trace d'un commentaire qui pretendait a tort que le module compilait
> sans ACE. A trancher au moment de l'ecrire, pas avant.

## Verification

Au lancement, le journal doit porter :

```
LogPluginManager: Mounting Project plugin ACE_ASR
LogACEASR: ACE_ASR module started.
```

et **rien d'autre** tant que l'evaluation n'a pas commence : aucun
chargement de modele, aucune session ASR. VRAM relevee avant / apres
activation : 2609 -> 2662 Mio.

## Les autres archives NVIDIA, non installees

`ACE_TTS_v5_7_1.0.4.zip` (1,96 Go) et `ACE_LLM_5.7_v0.5.zip` (4,05 Go)
sont telechargees mais non evaluees. La voix a ete tranchee autrement
(voir `docs/VOIX-DECISION.md`) et le LLM tourne dans llama.cpp, hors du
moteur, pour l'isolation de crash que reclame une borne.
