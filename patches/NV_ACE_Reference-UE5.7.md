# Portage du plugin NVIDIA ACE vers Unreal Engine 5.7

Le plugin `NV_ACE_Reference` v2.5.0rc3 n'est publie que pour UE **5.4, 5.5 et
5.6**. Le projet tourne sur **5.7**. Ce document consigne ce qu'il a fallu
faire, parce que le plugin pese 4 Go et n'est pas versionne — a la prochaine
reinstallation, tout ceci serait perdu.

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
