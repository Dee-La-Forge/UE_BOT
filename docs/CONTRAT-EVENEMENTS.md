# Contrat d'evenements — Unreal ↔ sidecar

WebSocket sur `127.0.0.1:8765`. Messages JSON, sauf l'audio qui transite en
binaire (voir plus bas).

Principe : **le sidecar ne connait rien de la scenographie.** Il ignore ce
qu'est un glitch, un tampon ou un panneau de sortie. Il emet des faits ;
Unreal decide de la mise en scene. Cette separation permet de retoucher la
scenographie sans toucher a l'IA, et inversement.

## Unreal → sidecar

| Evenement | Charge | Emis quand |
|---|---|---|
| `presence.detectee` | `{}` | `BP_LidarManager` passe sous le seuil de distance |
| `presence.perdue` | `{}` | `AbsenceCount` depasse le seuil |
| `audio.visiteur` | *binaire PCM16 16 kHz mono* | SileroVAD a clos un segment de parole |
| `session.reset` | `{}` | abandon, timeout, ou remise a zero manuelle |

## sidecar → Unreal

| Evenement | Charge | Declenche cote Unreal |
|---|---|---|
| `session.demarree` | `{"avatar": "germain"}` | **glitch + switch avatar** |
| `parole.debut` | `{"texte": "...", "emotion": "Concerned"}` | pose faciale via `E_Emotions` |
| `parole.audio` | *binaire PCM16 24 kHz mono* | lecture + relais NeuroSync |
| `parole.fin` | `{}` | retour a l'idle d'ecoute |
| `verdict` | `{"decision": "ACCEPTE"}` | **tampon** `stamp_accepted` / `stamp_refused` |
| `session.terminee` | `{}` | **panneau « quittez la zone »** (`Exit_Stamp`) |
| `erreur` | `{"code": "...", "repli": "..."}` | mode degrade — voir plus bas |

## Sequence nominale

```
Unreal                          sidecar
  │                                │
  ├─ presence.detectee ───────────►│
  │◄──────────── session.demarree ─┤   → GLITCH + SWITCH AVATAR
  │◄──────────────── parole.debut ─┤   → emotion
  │◄──────────────── parole.audio ─┤   → lecture + NeuroSync
  │◄──────────────────  parole.fin ─┤
  │                                │
  ├─ audio.visiteur ──────────────►│   (× 5 a 10 tours)
  │◄──────────── parole.debut/… ───┤
  │                                │
  │◄───────────────────── verdict ─┤   → TAMPON accepte | refus
  │◄─────────────  session.terminee ┤   → PANNEAU "quittez la zone"
  │                                │
  ├─ presence.perdue ─────────────►│   → RESET, retour en veille
```

## Audio

Les trames audio passent en **frames binaires WebSocket**, pas en JSON
base64 : l'encodage couterait ~33 % de volume et une allocation par trame,
sur un chemin ou chaque milliseconde compte.

Chaque frame binaire est precedee du message JSON qui la decrit
(`parole.audio` avec `seq` et `taux`), de sorte que le recepteur sache
toujours a quoi rattacher les octets qui suivent.

| Sens | Format |
|---|---|
| visiteur → sidecar | PCM16, **16 kHz**, mono (ce qu'attend Whisper) |
| sidecar → Unreal | PCM16, **24 kHz**, mono (sortie Piper, entree NeuroSync) |

## Mode degrade

La borne doit continuer a fonctionner meme si le sidecar meurt. C'est la
raison d'etre du decoupage en processus separes.

| Panne | Comportement attendu |
|---|---|
| WebSocket ferme | Unreal rejoue les repliques de `repli:` (`agent.yaml`) |
| LLM injoignable | `erreur` avec `repli` — l'agent dit une phrase generique |
| NeuroSync injoignable | bascule sur les 25 poses `MHF_*` (phonemes) |
| STT muet | `repli.incompris` — l'agent fait repeter |

Aucune de ces pannes ne doit figer l'ecran ni bloquer un visiteur dans la
zone. **Le pire scenario acceptable est un agent laconique ; jamais un agent
muet ou une borne gelee.**

## Ce qui reste a decider

- **Le choix de l'avatar** : cote Unreal (`RandomInteger`, comme
  aujourd'hui) ou cote sidecar, qui connait les personas disponibles ?
  Trancher une fois le second Narrative Design recupere — le nombre reel de
  personas conditionne la reponse.
- **Le glitch de fin** : les captures ne montrent pas s'il y a un second
  glitch au reset, ou seulement a l'entree en session.
