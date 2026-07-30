# Portage du plugin NVIDIA ACE vers Unreal Engine 5.7

## PREREQUIS MATERIEL : Audio2Face exige une carte RTX

Constate le 30/07/2026 sur un poste equipe d'une **GTX 1060** (Pascal),
plugin correctement installe et charge :

```
LogACECore:        Loaded ACE plugin version 2.5.0-20250614-2282
LogACEAimWrapper:  AIM feature 0x2e578b not available (No supported hardware found)
LogACEA2FLocal:    Unable to load AIM Audio2Face-3D local execution feature,
                   LocalA2F-James provider won't be available
```

Le moteur d'inference de NVIDIA (AIM/Nvigi) ne trouve pas de materiel
compatible : Pascal n'a pas de coeurs Tensor, et le modele de diffusion
3.0 en depend. **Aucun reglage ni patch n'y changera quoi que ce soit** —
le montage etait bon, c'est le GPU qui manque. Sur la borne (RTX 3090 Ti),
le meme plugin s'initialise normalement (voir « Etat au 29/07/2026 »).

Consequences pratiques :

- sur une machine sans RTX, la borne fonctionne en **mode degrade** :
  `OuvrirSessionA2F` ne trouve aucun fournisseur `LocalA2F*`, journalise
  l'avertissement prevu, et la voix passe par `AgentVoiceComponent` —
  l'agent parle, bouche fermee. C'est un chemin utile a tester en soi ;
- **la validation du lipsync ne peut se faire que sur la borne.** Le
  chercher ailleurs fait perdre une soiree.

Ne pas confondre ce cas avec l'echec silencieux documente plus bas
(`nvaim.core.framework.dll` introuvable) : celui-la vient d'une copie
incomplete du plugin, celui-ci du materiel. Le journal les distingue —
« Failed to load ... dll » d'un cote, « No supported hardware found » de
l'autre.

---


Le plugin `NV_ACE_Reference` v2.5.0rc3 n'est publie que pour UE **5.4, 5.5 et
5.6**. Le projet tourne sur **5.7**. Ce document consigne ce qu'il a fallu
faire, parce que le plugin pese 4 Go et n'est pas versionne — a la prochaine
reinstallation, tout ceci serait perdu.

## Face_AnimBP : la piece qui ne vient d'aucun des deux projets

`UAvatarSwitcherComponent::PreparerAudio2Face` impose au maillage facial
l'AnimBP suivant, au spawn de chaque avatar :

```
/Game/MetaHumans/Common/Face/Face_AnimBP.Face_AnimBP_C
```

Il porte le noeud `ApplyACEAnimation` et la correspondance ARKit — c'est
lui qui recoit la pose d'Audio2Face. **Il ne vient NI des MetaHumans, NI
de l'ancien projet** : il est copie du projet d'exemple Kairos de NVIDIA,

```
https://developer.nvidia.com/downloads/assets/ace/aceunrealsample-1.0.0.7z
```

et depose a la main dans `Content/MetaHumans/Common/Face/`. Or ce dossier
est **hors git** (les 3,4 Go d'assets migres). Consequence : une remise a
plat le perd, et rien ne le reclame avant le premier spawn.

Symptome, releve le 30/07/2026 sur un poste remonte de zero :

```
LogGardeFrontiere: Error: Avatars : AnimBP facial introuvable
                   (/Game/MetaHumans/Common/Face/Face_AnimBP.Face_AnimBP_C)
LogGardeFrontiere: Warning: Visage : 'Neutral' absent de Convai_MetaHuman_FaceAnim_C
```

Le second message est la consequence du premier : faute de Face_AnimBP,
le maillage reste sur `Convai_MetaHuman_FaceAnim`, dont les variables
d'emotion ne portent pas les noms attendus — le visage ne bouge plus du
tout, ni par ACE ni par les emotions.

Sur une machine sans RTX (voir ci-dessus), l'absence est sans consequence
visible puisque A2F ne tourne pas. **Sur la borne, elle est bloquante.**

## Origine des paquets

Telechargement depuis `developer.nvidia.com`, **compte connecte requis** :
`curl` ne recoit sinon qu'une page de connexion de 11 Ko.

```
nv_ace_reference-ue5.6-v2.5.0rc3.zip     3,2 Go   le plugin, sources comprises
ace_3.0_a2f_models.zip                   8,6 Go   contient les trois modeles
```

Le second est un conteneur : il renferme six archives, Claire / James / Mark
en versions 5.5 et 5.6. **Les trois modeles sont identiques en structure** —
71 fichiers chacun, meme arborescence, moins de 0,01 % d'ecart de taille. Ce
ne sont pas trois produits differents mais un meme reseau entraine sur trois
acteurs : seul le style d'articulation change. `James` a ete retenu.

## Installation

1. Copier `NV_ACE_Reference/` dans `unreal/Plugins/`, **en excluant
   `Binaries/` et `Intermediate/`** — compiles pour 5.6, ils ne se
   chargeraient pas et masqueraient l'erreur reelle.
2. Extraire `NvAudio2FaceJames-UE5.6-v2.4.0.zip` dans `unreal/Plugins/`,
   puis y supprimer `Binaries/` et `Intermediate/` de la meme facon.
3. Dans les deux `.uplugin`, retirer ces deux champs :

   ```json
   "EngineVersion": "5.6.0",
   "Installed": true,
   ```

   `EngineVersion` fait refuser le chargement sur 5.7. `Installed` marque le
   plugin comme fourni avec le moteur, avec binaires precompiles — ce qu'il
   n'est plus des lors qu'on le recompile.

## Le seul correctif de code necessaire

Douze modules sur treize compilent tels quels. Un seul echoue :

```
ACECoreModule.cpp(91,45): error C2039:
'FindOrAddSection' n'est pas membre de 'FConfigFile'
```

`FConfigFile::FindOrAddSection` a disparu en 5.7. Son remplacant public,
`FindOrAddConfigSection`, rend un pointeur **const** : on ne peut plus y
ajouter d'entrees. NVIDIA avait laisse un `TODO: fix deprecation warnings
for FindOrAddSection` juste au-dessus de l'appel — l'echeance etait connue.

Le bloc concerne migre les reglages d'un ancien plugin ACE 2.0/2.1 vers le
nouveau nom de section. Sur une installation neuve il ne s'execute jamais,
mais il doit compiler.

Dans `Source/ACECore/Private/ACECoreModule.cpp`, ajouter une branche 5.7
**avant** la branche 5.4 existante :

```cpp
#if !UE_VERSION_OLDER_THAN(5,7,0)
        const FConfigSection* OldSection = GConfig->GetSection(*OldSectionName, false, DefaultConfigPath);
        if (OldSection != nullptr)
        {
            for (const TPair<FName, FConfigValue>& Entry : *OldSection)
            {
                GConfig->SetString(*NewSectionName, *Entry.Key.ToString(),
                    *Entry.Value.GetSavedValue(), DefaultConfigPath);
            }
            GConfig->EmptySection(*OldSectionName, DefaultConfigPath);
            ACESettings->LoadConfig(ACESettings->GetClass(), *DefaultConfigPath, EPropertyPortFlags::PPF_None);
        }
#elif !UE_VERSION_OLDER_THAN(5,4,0)
        ... code d'origine inchange ...
#else
```

On passe par `GConfig->SetString`, cle par cle. Equivalent pour une
migration de reglages : seules des valeurs multiples portant le meme nom
seraient aplaties, ce qui ne concerne aucun reglage d'`ACESettings`.

## Pourquoi le portage etait previsible

`Source/` ne pese que **8,8 Mo**, contre **3,5 Go** de `ThirdParty/`. Le
plugin n'est qu'une fine enveloppe Unreal autour du SDK natif de NVIDIA, et
c'est ce SDK qui fait le travail. Une enveloppe mince traverse un saut de
version bien plus facilement qu'un plugin qui reimplemente le moteur.

## Ce qui n'a PAS ete fait

Les avertissements de deprecation subsistent, notamment `FInputGesture` dans
`OmniverseLiveLinkCommands.cpp`. Ils n'empechent rien aujourd'hui ; ils
casseront a une version future. A reprendre si le plugin doit vivre
longtemps sur ce projet.

## Piege de l'installation : ne pas exclure tous les dossiers `Binaries`

Le plugin porte DEUX sortes de `Binaries`, et elles n'ont rien a voir :

```
NV_ACE_Reference/Binaries/                       artefacts UE 5.6 — a exclure
NV_ACE_Reference/ThirdParty/Nvigi/Binaries/      1,1 Go de natif NVIDIA — INDISPENSABLE
```

Un `robocopy /XD Binaries` exclut les deux, par nom, a tous les niveaux de
l'arborescence. Le plugin se charge alors sans erreur de compilation, et
echoue silencieusement au demarrage :

```
LogWindows: Failed to load '.../ThirdParty/Nvigi/Binaries/Win64/nvaim.core.framework.dll'
(GetLastError=126)
```

`nvaim.core.framework.dll` est le coeur du moteur d'inference. Sans lui,
aucune animation faciale, et rien d'autre ne le signale.

**Exclure `Binaries` et `Intermediate` uniquement a la RACINE du plugin.**

Verification apres copie : `ThirdParty/` doit peser **3,5 Go**. S'il n'en
fait que 2,4, les binaires natifs manquent.

## Etat au 29/07/2026

Le plugin se charge et s'initialise sur UE 5.7 :

```
Loaded NvAudio2FaceJames plugin version 2.4.0-20250610-2270
Loaded AIM core DLL from .../nvaim.core.framework.dll
Found adapter 'NVIDIA GeForce RTX 3090 Ti'  # shader GFLOPS: 41932.80
```

**Reste ouvert :** `Content/MetaHumans/Common/Animation/ABP_MH_LiveLink`
echoue a compiler au demarrage. Cause racine, en tete de cascade :

```
VerifyImport: Failed to find script package for import object 'Package /Script/LiveLink'
VerifyImport: Failed to find script package for import object 'Package /Script/LiveLinkGraphNode'
```

L'asset est charge a 12:31:23, soit 18 secondes avant l'initialisation du
moteur — les modules ACE se chargent en `PreDefault` et `PreLoadingScreen`,
donc avant le module runtime de LiveLink. L'asset n'est pas utilise par le
projet aujourd'hui, mais c'est precisement l'AnimBP qui recevra la pose
LiveLink d'Audio2Face. A traiter avant de cabler le visage.

## ABP_MH_LiveLink : hypothese testee, ecartee

Depuis l'installation d'ACE, `Content/MetaHumans/Common/Animation/ABP_MH_LiveLink`
echoue a compiler au demarrage. Zero occurrence dans les journaux
anterieurs : le plugin en est bien la cause.

Cause racine, en tete de la cascade d'une vingtaine de messages :

```
VerifyImport: Failed to find script package for import object 'Package /Script/LiveLink'
VerifyImport: Failed to find script package for import object 'Package /Script/LiveLinkGraphNode'
```

L'asset est charge dix-huit secondes avant l'initialisation du moteur.

**Hypothese** : les modules d'ACE se chargent en `PreDefault` et
`PreLoadingScreen`, ceux de LiveLink tous en `Default`. `ACEGraphNode` en
`PreDefault` enregistrerait ses noeuds d'AnimGraph avant que
`/Script/LiveLinkGraphNode` n'existe.

**Test** : `ACEGraphNode` passe de `PreDefault` a `Default`.

**Resultat** : aucun effet. Vingt occurrences, identiques. ACE continue de
se charger normalement — le changement etait sans danger, mais sans
benefice. **Revenu a la valeur d'origine de NVIDIA.**

Le declencheur est donc ailleurs : `AIMWrapper` en `PreLoadingScreen`, ou
un effet de bord du montage des assets du plugin.

### Pourquoi on n'insiste pas

L'asset est reference par les trois avatars, donc non supprimable. Mais
depuis le montage Audio2Face, `UAvatarSwitcherComponent::PreparerAudio2Face`
impose `Face_AnimBP` au maillage facial **au spawn**. Le role de
`ABP_MH_LiveLink` a l'execution est donc repris.

Ce qui reste est du bruit au demarrage de l'editeur, sur un asset dont la
fonction est superseded. A reprendre si un defaut visuel apparaissait sur
un avatar — ce serait alors le premier endroit ou regarder.

## Second correctif : exposer la session de flux

L'API publique n'accepte qu'un `USoundWave` **cuit**. `SoundWaveConversion.cpp`
appelle `InitAudioResource` puis `ReadCompressedInfo` sur les donnees
compressees de l'asset. Il existe un chemin de secours lisant `RawPCMData`,
desactive par defaut, que NVIDIA commente ainsi :

> *This might work. Really we're just hoping to get lucky and find something
> useful in your USoundWave's RawPCMData* — *there's a race condition here*

Ce chemin convient a Kairos, qui enregistre un clip au micro puis l'anime.
Il ne convient pas a une borne qui synthetise sa voix en continu.

`FAudio2XSession` est l'interface juste :

```cpp
StartSession(IACEAnimDataConsumer*)
SendAudioSamples(TArrayView<const int16>, bEndOfSamples, emotion, params)
EndAudioSamples()
```

Elle prend des trames int16 au fil de l'eau — ce que le sidecar produit — et
reechantillonne elle-meme vers les 16 kHz du reseau.

**Le correctif tient en deux gestes**, tout le reste etant deja public
(`GetProviderFromName` et `GetDefaultProviderName` sont exportes dans
`Public/ACERuntimeModule.h`, `A2FProvider.h` et `ACETypes.h` sont dans
`ACECore/Public/`) :

1. Deplacer `ACERuntime/Private/A2XSession.h` vers `ACERuntime/Public/`
2. Exporter la classe : `class ACERUNTIME_API FAudio2XSession`

### Piege cote projet

`TUniquePtr<FAudio2XSession>` sur une declaration en avant ne compile pas :
le code genere par UHT detruirait un type incomplet, et un destructeur
declare a la main entre en collision avec celui que genere UHT.

Utiliser **`TPimplPtr`**, fait pour ce cas : il capture le destructeur a la
construction, la ou le type est complet. Aucun destructeur a declarer.

### Ne pas toucher aux phases de chargement d'ACE

Deux tentatives, deux echecs. Consignes pour qu'on ne les refasse pas.

| Modification | Resultat |
|---|---|
| `ACEGraphNode` : PreDefault → Default | aucun effet, erreur identique |
| `AIMWrapper` : PreLoadingScreen → Default | **PLANTAGE au demarrage du moteur** |

`AIMWrapper` initialise le framework d'inference NVIDIA, et quelque chose en
depend avant la phase `Default`. Le decaler fait tomber l'editeur avant
meme l'ecran de chargement.

**Les phases de NVIDIA sont a laisser telles quelles.**

### La voie qui reste pour ABP_MH_LiveLink

Ne pas chercher a le faire compiler : couper ce qui le charge.

Les trois avatars le referencent comme classe d'animation de leur maillage
`Face`. Or `UAvatarSwitcherComponent::PreparerAudio2Face` impose
`Face_AnimBP` a ce meme maillage des le spawn : la reference d'origine ne
sert plus qu'a faire charger un asset casse au demarrage de l'editeur.

Remplacer `ABP_MH_LiveLink` par `Face_AnimBP` dans les trois Blueprints
d'avatar, et plus rien ne le chargera.

### RESOLU — et ACE n'y etait pour rien

La cause etait dans le projet, pas dans le plugin.

`UAvatarSwitcherComponent`, dans son CONSTRUCTEUR :

```cpp
ConstructorHelpers::FClassFinder<AActor> Trouve(".../BP_AgentGermain_C");
```

`FClassFinder` charge de facon synchrone. Dans un constructeur, cela se
produit a la creation du CDO — pendant l'initialisation du module, avant que
tous les plugins soient debout. Charger un MetaHuman tire toutes ses
dependances, dont `ABP_MH_LiveLink`, dont les imports reclament
`/Script/LiveLink`, pas encore en memoire.

**ACE a revele le defaut, il ne l'a pas cree.** Ses quatre modules en
`PreDefault` et `PreLoadingScreen` ont deplace l'ordre d'initialisation ;
avant, il tombait juste par chance. C'est pourquoi les deux tentatives sur
ses phases de chargement etaient vouees a l'echec : elles repondaient a la
mauvaise question.

**La bonne question n'etait pas « pourquoi LiveLink est-il en retard »
mais « pourquoi cet asset est-il charge si tot ».**

Correction : `TArray<FSoftClassPath>` resolu dans `BeginPlay`. Resultat
mesure — 20 occurrences avant, **0 apres**, ACE inchange.

`ConstructorHelpers` convient a un asset simple. Pas a un MetaHuman.
