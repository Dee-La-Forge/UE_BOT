# Lipsync — NeuroSync est verrouille par Convai

Constat du 27/07/2026, en tentant l'integration.

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
