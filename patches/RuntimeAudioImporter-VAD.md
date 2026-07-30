# Extension VAD du plugin RuntimeAudioImporter (creation maison)

Le micro de la borne repose sur une architecture de fournisseurs VAD qui
n'existe dans AUCUNE distribution de RuntimeAudioImporter — ni le depot
GitHub (archive fevrier 2025), ni ses forks. C'est une **creation du
projet** : une classe de base ajoutee dans le plugin, et un fournisseur
Silero dans un plugin dedie.

L'installation d'origine (juillet 2026) a ete perdue avec le dossier
Plugins ; ce qui suit permet de la remonter en quelques minutes.

## Les trois pieces

1. **`URuntimeVADProviderBase`** — la classe de base, a poser dans le
   plugin RuntimeAudioImporter :

   ```
   Plugins/RuntimeAudioImporter/Source/RuntimeAudioImporter/Public/VAD/RuntimeVADProviderBase.h
   ```

   Copie de reference versionnee ICI : `patches/RuntimeVADProviderBase.h`
   (reconstruite le 30/07/2026 depuis ses deux consommateurs, qui en fixent
   le contrat : les overrides de RuntimeSileroVADProvider.h et l'usage de
   UVisitorMicComponent — ProcessAudio rend negatif=erreur, 0=silence,
   positif=parole).

2. **`RuntimeAudioImporterSileroVAD`** — le plugin fournisseur (code
   projet, ~2,5 Mo avec le modele) : Silero VAD via ONNX Runtime, modele
   dans `Content/SileroVADModel.uasset`. Sur Win64 + UE >= 5.6, il s'appuie
   sur les modules moteur NNERuntimeORT/NNEOnnxruntime — rien a embarquer.
   Copie de travail retrouvee dans l'ancien projet
   (`GardeFrontiereByDboy/Plugins/`) et reposee dans `unreal/Plugins/`.

3. **`UVisitorMicComponent`** (module GardeFrontiere, versionne) —
   instancie le fournisseur PAR CHEMIN DE CLASSE
   (`/Script/RuntimeAudioImporterSileroVAD.RuntimeSileroVADProvider`) :
   le module compile sans le plugin Silero, mais PAS sans la classe de
   base (UPROPERTY type dur, VisitorMicComponent.h).

## Remontage apres reinstallation de RuntimeAudioImporter

```
copy patches\RuntimeVADProviderBase.h ^
  unreal\Plugins\RuntimeAudioImporter\Source\RuntimeAudioImporter\Public\VAD\
```

puis reposer le plugin RuntimeAudioImporterSileroVAD dans unreal/Plugins/.
C'est tout — aucune autre modification du plugin hote n'est necessaire.

## Reste ouvert : `SetVADProvider` et l'ancien cablage Blueprint

`BP_FirstPersonCharacter` (migre de l'ancien projet) appelle **deux**
fonctions de l'extension d'origine :

| Appel Blueprint | Etat |
|---|---|
| `IsSpeechActive` sur `RuntimeVADProviderBase` | **restaure** — le Blueprint attestait qu'elle etait BlueprintCallable, l'exposition est reconstruite |
| `SetVADProvider` sur `StreamingSoundWave` | **NON restaure** |

`SetVADProvider` n'a pas ete reconstruit volontairement : les broches
(`Target`, `Provider`) donnent la signature, mais rien ne dit ce que la
fonction FAISAIT — on ecrirait du code au hasard dans un plugin tiers.

Et surtout, **plus personne n'en a besoin** : depuis le passage en C++,
`UVisitorMicComponent` possede tout le chemin VAD. Il instancie le
fournisseur Silero par nom de classe (`PreparerVAD`) et appelle
`ProcessAudio` trame par trame ; il ne delegue rien a
`StreamingSoundWave`. Ce cablage Blueprint est l'ANCIENNE methode.

**Correction recommandee : supprimer le noeud `Set VADProvider` de
`BP_FirstPersonCharacter`.** Tant qu'il est la, le Blueprint refuse de
compiler (5 erreurs au demarrage, relevees le 30/07/2026) — pour une
fonctionnalite que le C++ assure deja.

## Le dialogue « designed for build 5.5.0 »

Au demarrage, l'editeur demande :

> The 'RuntimeAudioImporter' plugin was designed for build 5.5.0.
> Attempt to load it anyway?

**Repondre oui** — et le champ a ete retire des `.uplugin` le 30/07/2026
pour que la question ne revienne plus. `EngineVersion` n'est qu'une
etiquette declarative : ces plugins sont fournis avec leurs sources et
**recompiles contre 5.7** a chaque build du projet. Le meme geste avait
ete fait pour NV_ACE_Reference (declare 5.6) — voir
`patches/NV_ACE_Reference-UE5.7.md`.

A refaire apres toute reinstallation depuis Fab ou le marketplace, qui
remet le champ.

## Lecon

Une extension maison logee dans un plugin tiers non versionne disparait
avec lui. La copie de reference vit desormais dans patches/, versionnee ;
si l'extension evolue, METTRE A JOUR patches/RuntimeVADProviderBase.h dans
le meme commit.
