# La voix de l'agent — etat au 30/07/2026

**Valide a l'ecoute, a reprendre en phase de finitions.**

## Ce qui a ete essaye, et ce que l'oreille a dit

| Piste | Verdict |
|---|---|
| Piper `fr_FR-siwis` (en service) | accent correct, **robotique** |
| Piper `tom`, `upmc`, `gilles` | meilleures, **toujours robotiques** |
| Prosodie debit/volume par emotion | brise la monotonie, ne cree pas d'intention |
| Chatterbox turbo, voix generique | **accent americain** |
| Chatterbox multilingue `fr`, voix generique | **accent canadien** prononce |
| Chatterbox `exaggeration` 0 / 0,5 / 1 | **difference non perceptible** en francais |
| **Chatterbox + CLONAGE** | **« c'est parfait les voix »** |
| **Texte porteur de jeu** (hesitations, souffles) | **« pas mal du tout, je valide »** |

## Les deux conclusions

**1. L'humanite vient de l'echantillon, pas du moteur.** Aucune voix
generique n'a convaincu ; toutes les voix clonees ont convaincu
immediatement. Le clonage n'est donc pas un raffinement : c'est la
condition d'entree.

**2. L'emotion vient du TEXTE, pas d'un curseur.** `exaggeration` ne
s'entend pas en francais. En revanche, une replique ECRITE avec des
hesitations s'entend tout de suite :

```
a  « Vous dites Lyon. Le motif de votre visite. »          plat
c  « Hhh... bon. Alors, vous dites Lyon, hein. Le motif ? » vivant
```

Or **notre grammaire GBNF interdit aujourd'hui la seconde forme** : la
classe `car` exclut les points de suspension, et `question` impose un
plancher de 25 caracteres pour empecher les questions telegraphiques.
Ces regles ont ete ecrites pour discipliner un 3B qui derivait ; elles
ont du meme coup supprime toute la matiere du jeu.

## Ce qu'il restera a faire

1. **Rouvrir la grammaire** : autoriser `…` dans `car`, permettre a la
   phrase de reaction de porter une interjection (« Hm. », « Bon. »).
   Attention : `_FIN_DE_PHRASE` (llm.py) traite `…` comme une fin de
   phrase et couperait le morceau TTS au milieu de l'hesitation — l'en
   retirer en meme temps.
2. **Inviter le prompt a jouer** : souffler devant une reponse evasive,
   marquer un temps avant un verdict. L'emotion viendrait alors de
   l'interpretation du dialogue par le LLM, ce qui est bien plus juste
   qu'un reglage global.
3. **Trancher la vitesse SUR LA BORNE.** Chatterbox tourne a x0,5 le
   temps reel sur la GTX 1060 (carte libre) contre x4 a x12 pour Piper.
   La 3090 Ti changera la donne, mais elle seule peut le dire.
4. **Choisir la voix definitive** et regler la question des droits (voir
   ci-dessous).

## Droits et vie privee

Les echantillons de reference sont **hors depot** (`.gitignore`) : ce
sont des enregistrements de personnes, et le depot est public. Une voix
publiee ici serait telechargeable et clonable par quiconque.

Pour l'exploitation, la voix retenue doit etre une voix **dont on
detient les droits** : celle d'un comedien avec accord ecrit, ou une
voix de synthese dont la licence autorise le clonage. Cloner la voix
d'un tiers sans son consentement n'est pas envisageable pour une
installation publique.
