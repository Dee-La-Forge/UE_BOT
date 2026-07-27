# Lipsync — etat de l'integration NeuroSync

> **Mise a jour du 27/07/2026, apres tentative complete.**
> Le modele a ete obtenu et l'architecture reimplementee : les poids se
> chargent, l'inference tourne vite, mais **le modele ne repond pas a
> l'audio**. Voir « Resultat de l'integration » en fin de document.
> Licence clarifiee : le modele est en **double licence MIT sous 1 M$ de
> CA**, ce qui est plus favorable que le CC BY-NC porte par leur code.

## Ce qu'on a trouve

| Ressource | Etat |
|---|---|
| `huggingface.co/convaitech/NEUROSYNC` | **HTTP 200, `gated=auto`** — telechargement refuse sans compte |
| `huggingface.co/AnimaVR/NEUROSYNC` | **307** — redirige vers convaitech |
| `huggingface.co/AnimaVR/NEUROSYNC_Audio_To_Face_Blendshape` | **307** — redirige |
| `github.com/AnimaVR/NeuroSync_Local_API` | **404** |
| `github.com/AnimaVR/NeuroSync_Real-Time_API` | **404** |
| Compte GitHub AnimaVR | 4 depots restants, **aucun NeuroSync** |

Tentative de telechargement direct :

```
HTTP/1.1 401 Unauthorized
X-Error-Code: GatedRepo
X-Error-Message: Access to model convaitech/NEUROSYNC is restricted.
```

Le fichier recu pesait 128 octets — le message d'erreur, pas le modele.

## Interpretation

**Convai a absorbe NeuroSync.** Les depots d'origine d'AnimaVR ont disparu
de GitHub, les modeles HuggingFace redirigent vers le compte `convaitech`,
et l'acces y est desormais conditionne a une approbation.

C'est le point qui merite l'attention : **le chantier vise a se defaire de
Convai, et la meilleure option de lipsync appartient maintenant a Convai.**
S'y appuyer reintroduirait exactement la dependance qu'on cherche a
supprimer — non plus au moment de l'execution, mais au moment de
l'approvisionnement, ce qui est plus insidieux : un modele qu'on n'a plus
le droit de retelecharger est un modele qu'on ne peut plus deployer sur une
seconde borne, ni restaurer apres une panne disque.

## Options

### A. Obtenir l'acces (rapide, mais dependant)

`gated=auto` signifie approbation automatique : il suffit d'un compte
HuggingFace connecte, d'accepter les conditions, et l'acces est accorde
immediatement. Puis un token dans l'environnement.

- Cout : ~942 Mo (modele) + ~2,5-3 Go (PyTorch CUDA)
- Risque : conditions revocables, licence commerciale au-dela d'1 M$ de CA
- **Si cette voie est retenue : archiver le `.pth` hors ligne des
  l'obtention**, pour ne pas dependre d'un retelechargement futur.

### B. S'en tenir aux visemes MHF_* (deja fait, deja mesure)

Le repli construit lors du prototypage n'est plus un repli : c'est une
solution complete et autonome.

- Piper expose les phonemes **et leurs durees** (`num_samples`)
- Mapping vers les 25 poses `MHF_*` du plugin Convai — deja presentes
- Frise verifiee : **0 ms de derive** sur la duree de l'audio
- Cout : **0 Mo, 0 Go de VRAM, 0 ms de latence supplementaire**
- Aucune dependance externe, rien a retelecharger, rien a approuver

Limite : 15 formes de bouche contre 61 blendshapes ARKit. Pas de
coarticulation fine, pas d'expression du haut du visage, pas des 7 valeurs
d'emotion. En contrepartie, les emotions restent pilotables par le tag
`[EMOTION:...]` sur les 16 poses de `Motions2/Face/`.

### C. Chercher un equivalent libre

Audio2Face de NVIDIA (licence a verifier), ou un modele viseme entrainable.
Cout d'etude non nul, resultat incertain.

## Recommandation

**Commencer par B.** Elle est deja faite, mesuree, gratuite et sans
dependance. Elle permet d'avancer immediatement sur l'integration Unreal,
qui est le vrai chemin critique.

A reevaluer une fois la borne fonctionnelle : si le rendu labial deçoit a
l'usage, l'option A reste ouverte, et on saura alors precisement ce qu'on
achete en echange de la dependance.

Ne pas bloquer le chantier sur une approbation d'acces.

## Effet de bord utile

Le nettoyage du cache pip declenche pendant cette tentative a libere
**7,4 Go**. Disque passe de 11 a 18 Go libres.

---

# Resultat de l'integration (27/07/2026)

## Ce qui fonctionne

| | |
|---|---|
| Chargement `strict=True` | **reussi** — 235,5 M parametres, 1026 Mo de VRAM |
| Geometrie | 8 couches encodeur + 8 decodeur, cachee 1024, ff 4096, sortie 68 |
| Passe avant | (N, 68), cadence 60 fps exacte |
| Vitesse | 16 ms pour 2,13 s d'audio — **133x le temps reel** |

L'inference est donc **bien plus rapide que mon estimation** de 100-200 ms.
Si le modele fonctionnait, il n'ajouterait qu'une quinzaine de millisecondes.

## Ce qui ne fonctionne pas

**Le modele sort un visage quasi constant, quelle que soit l'entree.**

```
                         JawOpen moyen   ecart-type
parole (3,3 s)              0.065          0.010
silence (2,5 s)             0.062            —
entree entierement nulle    0.064          0.009
bruit aleatoire             0.069          0.012
```

Silence et parole sont indiscernables. La machoire ne bouge pas.

## Pistes ecartees par la mesure

| Hypothese | Test | Resultat |
|---|---|---|
| Mauvais nombre de tetes d'attention | balayage 2, 4, 8, 16, 32, 64 | aucun effet |
| Ordre de normalisation | post-norm vs pre-norm | aucun effet |
| Echelle des features | x1, x3, x10 | effet marginal |
| Dimensions utiles mal placees | bruit sur 0-68 vs 69-255 | les deux repondent faiblement |

## Ce que revele l'analyse des poids

La norme par colonne de `encoder.embedding` (1024 x 256) montre :

```
dims   0-68    0.55 a 1.91     signal, 45 a 58 sigma au-dessus du plateau
dims  69-255   0.6496 +/- 0.015  plateau plat = poids non entraines
```

Seules **69 dimensions portent du signal** — exactement les `23 MFCC x 3` du
code public. Les 187 autres sont du remplissage. Les features extraites sont
pourtant bien distinctes entre parole et silence (ecart absolu moyen 0,33),
donc le probleme n'est pas l'extraction.

## Diagnostic

Le checkpoint Convai **diverge du code public** sur des points non
recuperables depuis les poids : il annonce 8 couches la ou `config.py` en
declare 4, et 256 dimensions d'entree la ou le code en produit 69. D'autres
details — activation, RoPE actif ou non, facteur d'echelle, pretraitement —
peuvent differer sans qu'aucune verification de forme ne le signale.

`strict=True` garantit les noms et les dimensions. **Il ne garantit pas la
semantique** : c'est precisement le mode de defaillance rencontre ici.

## Decision

**Retour a l'option B : les visemes MHF_\*.** Elle fonctionne, elle est
mesuree a 0 ms de derive, elle ne coute rien et n'a aucune dependance.

Le module `src/neurosync.py` est conserve : il est correct sur tout ce qui
est verifiable, et une specification d'inference exacte suffirait a le
rendre operationnel.

**Pour debloquer un jour :** demander a Convai la specification d'inference
correspondant a *ce* checkpoint — featurisation exacte et details
d'architecture. Ils possedent le modele ; le code public lui est anterieur.
