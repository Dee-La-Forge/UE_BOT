#!/usr/bin/env bash
# Telechargement des modeles et binaires du sidecar.
#
# Reprend ou il s'est arrete (curl -C -), et saute ce qui est deja complet.
# Cible : ~2,8 Go. Verifier l'espace disque avant de lancer.

set -u
cd "$(dirname "$0")/.."
RACINE="$(pwd)"

LLAMA_TAG="b10152"
LLAMA_CUDA="12.4"   # compatible pilote 596.x ; plus large que 13.3

mkdir -p tools/llama.cpp models/llm models/piper

# recuperer <url> <destination> <taille_attendue_octets|0>
recuperer() {
  local url="$1" dest="$2" attendu="${3:-0}"
  local nom; nom="$(basename "$dest")"

  if [ -f "$dest" ] && [ "$attendu" != "0" ]; then
    local taille; taille=$(stat -c%s "$dest" 2>/dev/null || echo 0)
    if [ "$taille" = "$attendu" ]; then
      echo "  deja complet : $nom"
      return 0
    fi
    echo "  reprise      : $nom ($taille / $attendu)"
  fi

  echo "  telechargement : $nom"
  curl -L -C - --retry 3 --retry-delay 2 -# -o "$dest" "$url" || {
    echo "  ECHEC : $nom"
    return 1
  }
}

echo ""
echo "=== 1/3  llama.cpp $LLAMA_TAG (CUDA $LLAMA_CUDA) ==="
BASE="https://github.com/ggml-org/llama.cpp/releases/download/$LLAMA_TAG"
recuperer "$BASE/llama-$LLAMA_TAG-bin-win-cuda-$LLAMA_CUDA-x64.zip" \
          "tools/llama.cpp/llama.zip" 0
recuperer "$BASE/cudart-llama-bin-win-cuda-$LLAMA_CUDA-x64.zip" \
          "tools/llama.cpp/cudart.zip" 0

echo ""
echo "=== 2/3  Qwen2.5-3B-Instruct Q4_K_M (2,1 Go) ==="
recuperer "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf" \
          "models/llm/qwen2.5-3b-instruct-q4_k_m.gguf" 2104932768

echo ""
echo "=== 3/3  Voix Piper fr_FR-siwis-medium ==="
PIPER="https://huggingface.co/rhasspy/piper-voices/resolve/main/fr/fr_FR/siwis/medium"
recuperer "$PIPER/fr_FR-siwis-medium.onnx"      "models/piper/fr_FR-siwis-medium.onnx" 0
recuperer "$PIPER/fr_FR-siwis-medium.onnx.json" "models/piper/fr_FR-siwis-medium.onnx.json" 0

echo ""
echo "=== Decompression de llama.cpp ==="
cd tools/llama.cpp
for z in llama.zip cudart.zip; do
  [ -f "$z" ] && unzip -o -q "$z" && echo "  extrait : $z"
done
cd "$RACINE"

echo ""
if [ -f tools/llama.cpp/llama-server.exe ]; then
  echo "  llama-server.exe : OK"
else
  echo "  llama-server.exe : ABSENT — verifier l'extraction"
fi
du -sh models tools 2>/dev/null
echo ""
echo "=== Termine ==="
