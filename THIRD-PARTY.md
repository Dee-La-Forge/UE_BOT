# Composants tiers — licences et conditions

Inventaire pour l'exploitation museale. **Avant toute mise en service
commerciale, les deux points ⚠️ doivent etre tranches.** Etat au 30/07/2026.

| Composant | Usage | Licence | Usage commercial |
|---|---|---|---|
| **Qwen2.5-3B-Instruct** (GGUF) | LLM de l'entretien | **Qwen Research License** | ⚠️ **NON sans accord Alibaba.** Contrairement aux 7B/14B (Apache 2.0), le 3B exige une licence commerciale. Basculer sur Qwen2.5-1.5B ou 7B (Apache 2.0), ou obtenir l'accord — et consigner la decision ici. |
| llama.cpp (b10152) | Serveur d'inference LLM | MIT | oui |
| faster-whisper 1.0.3 | STT | MIT | oui (modeles Whisper : MIT, OpenAI) |
| Piper 1.6.0 | TTS | MIT | oui |
| Voix `fr_FR-siwis` | Voix TTS francaise | corpus SIWIS : **CC BY 4.0** | oui, **avec attribution** (crediter le corpus SIWIS dans le dispositif ou sa documentation) |
| NVIDIA ACE / Audio2Face-3D (NV_ACE_Reference 2.5.0rc3 + NvAudio2FaceJames) | Lipsync local | Licence NVIDIA (telechargement sous compte developpeur) | ⚠️ **a verifier** : conditions d'exploitation/redistribution pour une installation commerciale. Archiver les zips hors ligne (publies pour UE 5.4-5.6 seulement, portage 5.7 : voir `patches/`). |
| Plugin Convai (assets `Animations`) | AnimBP corps/visage, 16 poses d'emotions | proprietaire Convai | ⚠️ **zone grise** : le droit d'utiliser ces assets apres resiliation de l'abonnement est a verifier aupres de Convai avant de couper le compte. |
| MetaHuman (AgentGermain, Common) | Avatar | EULA Epic Games | oui, dans un produit Unreal Engine (conditions EULA UE) |
| Unreal Engine 5.7 | Moteur | EULA Epic Games | oui (redevances selon EULA ; une installation museale n'est generalement pas concernee par les royalties jeu, a confirmer selon le contrat) |
| SERIALCOM | Liaison serie LiDAR | plugin marketplace/communautaire — licence a confirmer | a verifier |
| RuntimeAudioImporter + SileroVAD | Capture micro + VAD | plugin Fab (achat) ; Silero VAD : MIT | oui (selon licence Fab standard) |
| websockets, httpx, pyyaml, numpy, sounddevice, requests | libs Python | BSD/MIT/Apache | oui |
| ~~NeuroSync~~ | ~~lipsync~~ | MIT sous 1 M$ CA, commerciale au-dela | **abandonne** (`docs/LIPSYNC-DECISION.md`) — plus une dependance. |

## Code du projet

Le depot lui-meme n'a pas de fichier LICENSE : **tous droits reserves** par
defaut. A trancher explicitement (et le consigner ici) si le depot doit
rester public.
