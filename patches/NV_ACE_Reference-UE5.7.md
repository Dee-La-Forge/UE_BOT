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
