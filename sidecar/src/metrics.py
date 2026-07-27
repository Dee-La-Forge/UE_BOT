"""Chronometrage du pipeline.

C'est l'instrument central du prototype : sans mesure par etage, on ne
saura pas quel maillon coute reellement, et on optimisera au hasard.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Mesure:
    """Releve de latence pour un tour de parole complet."""

    # Instant de reference : le visiteur vient de finir de parler.
    _origine: float = field(default_factory=time.perf_counter)
    _marques: dict[str, float] = field(default_factory=dict)

    def marquer(self, etape: str) -> float:
        """Enregistre l'instant de passage d'une etape, en ms depuis l'origine."""
        ecoule = (time.perf_counter() - self._origine) * 1000.0
        self._marques[etape] = ecoule
        return ecoule

    def depuis(self, etape_a: str, etape_b: str) -> float | None:
        """Duree entre deux marques, en ms."""
        if etape_a not in self._marques or etape_b not in self._marques:
            return None
        return self._marques[etape_b] - self._marques[etape_a]

    @property
    def temps_premier_son(self) -> float | None:
        """LE chiffre qui compte : delai avant que l'agent emette du son."""
        return self._marques.get("audio_premier_chunk")

    def resume(self) -> dict[str, float | None]:
        """Decomposition par etage, telle qu'on veut la lire."""
        return {
            "stt": self.depuis("debut", "stt_fin"),
            "llm_premier_token": self.depuis("stt_fin", "llm_premier_token"),
            "llm_premiere_phrase": self.depuis("llm_premier_token", "llm_premiere_phrase"),
            "tts_premier_chunk": self.depuis("llm_premiere_phrase", "tts_premier_chunk"),
            "lipsync": self.depuis("tts_premier_chunk", "lipsync_pret"),
            "TOTAL_premier_son": self.temps_premier_son,
            "total_tour_complet": self._marques.get("fin"),
        }

    def ecrire(self, chemin: Path, contexte: dict | None = None) -> None:
        """Ajoute le releve au journal de mesures (format JSONL)."""
        chemin.parent.mkdir(parents=True, exist_ok=True)
        ligne = {
            "horodatage": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "marques_ms": self._marques,
            "resume_ms": self.resume(),
        }
        if contexte:
            ligne["contexte"] = contexte
        with chemin.open("a", encoding="utf-8") as f:
            f.write(json.dumps(ligne, ensure_ascii=False) + "\n")

    def afficher(self) -> str:
        """Rendu lisible en console, pour le bench."""
        r = self.resume()
        lignes = ["", "  Decomposition de la latence", "  " + "-" * 42]
        etiquettes = {
            "stt": "STT (transcription)",
            "llm_premier_token": "LLM (1er token)",
            "llm_premiere_phrase": "LLM (1re phrase)",
            "tts_premier_chunk": "TTS (1er chunk)",
            "lipsync": "Lipsync (NeuroSync)",
        }
        for cle, etiquette in etiquettes.items():
            val = r.get(cle)
            if val is not None:
                lignes.append(f"  {etiquette:<24} {val:>8.0f} ms")
        lignes.append("  " + "-" * 42)
        total = r.get("TOTAL_premier_son")
        if total is not None:
            lignes.append(f"  {'PREMIER SON':<24} {total:>8.0f} ms")
        return "\n".join(lignes)
