# Contrat d'evenements — Unreal ↔ sidecar

WebSocket sur `127.0.0.1:8765`. Messages JSON, sauf l'audio qui transite en
binaire (voir plus bas).

Principe : **le sidecar ne connait rien de la scenographie.** Il ignore ce
qu'est un glitch, un tampon ou un panneau de sortie. Il emet des faits ;
Unreal decide de la mise en scene. Cette separation permet de retoucher la
scenographie sans toucher a l'IA, et inversement.

> Ce document decrit ce que le code FAIT, pas ce qu'il devrait faire. Mis a
> jour le 30/07/2026 apres audit croise des deux implementations
> (`SidecarClient.cpp` / `serveur.py`) — il avait derive des deux cotes.

## Connexion

Une seule connexion active a la fois : la borne est mono-poste. Si une
nouvelle connexion s'etablit (redemarrage d'Unreal) pendant que l'ancienne
agonise, le sidecar **libere la session, ferme l'ancienne socket** et sert la
nouvelle. Une deconnexion emporte la session en cours.

## Unreal → sidecar

| Evenement | Charge | Emis quand |
|---|---|---|
| `presence.detectee` | `{}` | ouverture de scene, une fois l'avatar en place (`OuvrirLaScene`) |
| *frame binaire* | PCM16 **16 kHz** mono, **sans descripteur JSON** | SileroVAD a clos un segment de parole du visiteur |
| `visiteur.silencieux` | `{}` | `DelaiReponseVisiteur` ecoule apres la fin d'une replique — l'agent relance |
| `session.reset` | `{}` | toute fin de session (`TerminerSession`) : depart, abandon, timeout |
| `presence.perdue` | `{}` | **jamais emis aujourd'hui** — accepte par le sidecar comme synonyme de `session.reset`, conserve pour compatibilite |

Le sidecar **ignore** toute frame binaire recue hors session (aucun
`presence.detectee` actif) : segment VAD parti apres la fin de session. Il
ne garde par ailleurs qu'**un tour en attente** au plus — les enonces
supplementaires arrives pendant une replique sont jetes.

## sidecar → Unreal

| Evenement | Charge | Cote Unreal |
|---|---|---|
| `session.demarree` | `{"avatar": "BP_AgentGermain"}` | journalise seulement — le glitch/switch a lieu en **fin** de session, zone vide |
| `visiteur.transcription` | `{"texte": "...", "duree": 2.4}` | **diagnostic pur** : ce que le STT a compris du dernier segment. Rien n'en depend — Unreal peut l'ignorer. Emis AVANT le `parole.debut` du tour qu'il declenche, pour que le journal donne la question avant la reponse. `texte` vide = rien compris (l'agent enchainera sur `repli.incompris`) |
| `parole.debut` | `{"texte": "...", "emotion": "Stare"}` | pose faciale pressentie via `E_Emotions` ; `bRepliqueEnCours` s'ouvre |
| `parole.audio` | `{"seq": 0, "taux": 22050, "texte": "...", "premier": true}` | retient `taux` pour la frame binaire qui suit |
| *frame binaire* | PCM16 mono, au `taux` annonce | lecture : Audio2Face (voix + visage), repli `Voix` sinon |
| `parole.fin` | `{}` | cloture du flux ; ACE finit de jouer son tampon |
| `emotion` | `{"valeur": "Angry"}` | emotion **definitive**, extraite du tag en fin de generation |
| `verdict` | `{"decision": "ACCEPTE"}` | `ACCEPTE`/`REFUSE` uniquement — tampon `stamp_accepted`/`stamp_refused` ; toute autre valeur est ignoree avec un Warning |
| `session.terminee` | `{}` | panneau « quittez la zone » apres `DelaiAvantSortie` |
| `erreur` | `{"code": "tts_indisponible", "repli": "..."}` | emis **seulement si le TTS du sidecar echoue lui-meme** ; Unreal journalise et passe en panne (`repli` est informatif, la replique de secours d'Unreal est locale) |

Nuance importante : une panne du **LLM seul** (TTS sain) ne produit **pas**
d'evenement `erreur` — le sidecar parle sa replique de repli `indisponible`
(agent.yaml) par le circuit normal `parole.debut/audio/fin`. Unreal n'a rien
a faire.

## Sequence nominale

```
Unreal                          sidecar
  │                                │
  ├─ presence.detectee ───────────►│
  │◄──────────── session.demarree ─┤   (journalise)
  │◄──────────────── parole.debut ─┤   → emotion pressentie
  │◄──────── parole.audio + trame ─┤   → Audio2Face / Voix
  │◄──────────────────  parole.fin ─┤
  │◄──────────────────── emotion ──┤   → emotion definitive
  │                                │
  ├─ trame binaire (visiteur) ────►│   (× questions_min a questions_max tours)
  │◄──────────── parole.debut/… ───┤
  │                                │
  │◄───────────────────── verdict ─┤   → TAMPON accepte | refus
  │◄─────────────  session.terminee ┤   → PANNEAU "quittez la zone"
  │                                │
  ├─ session.reset ───────────────►│   → liberation, retour en veille
```

## Audio

Les trames audio passent en **frames binaires WebSocket**, pas en JSON
base64 : l'encodage couterait ~33 % de volume et une allocation par trame,
sur un chemin ou chaque milliseconde compte.

| Sens | Format | Descripteur |
|---|---|---|
| visiteur → sidecar | PCM16, **16 kHz**, mono (ce qu'attend Whisper) | aucun — la frame part nue |
| sidecar → Unreal | PCM16, mono, au taux du champ `taux` (**22 050 Hz** pour `fr_FR-siwis-medium`) | `parole.audio` juste avant |

Le champ `taux` fait foi : ne rien coder en dur cote Unreal, la voix Piper
peut changer.

## Mode degrade

La borne doit continuer a fonctionner meme si le sidecar meurt. C'est la
raison d'etre du decoupage en processus separes.

| Panne | Comportement |
|---|---|
| WebSocket ferme / sidecar mort | Unreal parle ses repliques de secours (une seule annonce de panne par session), reconnexion en boucle |
| Sidecar gele, socket ouverte | watchdog `DelaiReponseSidecar` (10 s) : deconnexion forcee, puis chemin ci-dessus |
| LLM injoignable | le **sidecar** parle son repli `indisponible` (agent.yaml), circuit normal |
| STT muet / hors delai | repli `incompris` (« Repetez. ») ou `indisponible`, selon la panne |
| TTS mort | `erreur` avec `repli` — Unreal parle sa replique locale |
| Audio2Face absent | repli automatique : la voix joue par `AgentVoiceComponent`, bouche fermee |
| Etage STT/TTS fige | bornes de temps sidecar (20 s / 10 s) : panne franche + repli, jamais de gel |

Aucune de ces pannes ne doit figer l'ecran ni bloquer un visiteur dans la
zone. **Le pire scenario acceptable est un agent laconique ; jamais un agent
muet ou une borne gelee.**

## Ce qui reste a decider

- **Le choix de l'avatar** : cote Unreal (`RandomInteger`, comme
  aujourd'hui) ou cote sidecar, qui connait les personas disponibles ?
  Trancher une fois le second Narrative Design recupere — le nombre reel de
  personas conditionne la reponse.
- ~~Le glitch de fin~~ : tranche. Le glitch joue en **fin de session, zone
  vide uniquement** ; si la zone est encore occupee, la substitution est
  differee au depart du temoin.
