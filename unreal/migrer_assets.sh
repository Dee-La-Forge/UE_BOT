#!/usr/bin/env bash
# Migration des assets depuis l'ancien projet.
#
# Copie uniquement ce qui sert, et laisse derriere ce qui est regenerable
# (Intermediate, Binaries) ou mort (Grondin, Smith, PoliceUniform, presets
# d'eclairage). Idempotent : relancable sans dommage.

set -u
ANCIEN="/d/Archives/GardeFrontiereByDboy"
NOUVEAU="$(cd "$(dirname "$0")" && pwd)"

copier() {
  local src="$ANCIEN/$1" dst="$NOUVEAU/$2"
  if [ ! -e "$src" ]; then
    echo "  ABSENT   $1"
    return 1
  fi
  mkdir -p "$(dirname "$dst")"

  # `cp -r src dst` copie DANS dst quand dst existe deja, creant
  # AgentGermain/AgentGermain. Une premiere version du script le faisait,
  # et une relance dupliquait 3,4 Go en repertoires imbriques.
  # On copie donc le CONTENU de la source, pas la source elle-meme.
  if [ -d "$src" ]; then
    mkdir -p "$dst"
    cp -r "$src/." "$dst/" 2>/dev/null
  else
    cp -f "$src" "$dst" 2>/dev/null
  fi

  local taille; taille=$(du -sm "$dst" 2>/dev/null | cut -f1)
  printf "  ok  %-52s %5s Mo\n" "$1" "$taille"
}

echo ""
echo "=== 1/4  Plugin Convai — Source, Content, Resources ==="
echo "  (Intermediate et Binaries exclus : regeneres a la compilation)"
mkdir -p "$NOUVEAU/Plugins/Convai"
cp "$ANCIEN/Plugins/Convai/ConvAI.uplugin" "$NOUVEAU/Plugins/Convai/" 2>/dev/null && echo "  ok  ConvAI.uplugin"
copier "Plugins/Convai/Source"    "Plugins/Convai/Source"
copier "Plugins/Convai/Content"   "Plugins/Convai/Content"
copier "Plugins/Convai/Resources" "Plugins/Convai/Resources"

echo ""
echo "=== 2/4  Plugin SerialCOM ==="
mkdir -p "$NOUVEAU/Plugins/SerialCOM"
cp "$ANCIEN/Plugins/SerialCOM/SERIALCOM.uplugin" "$NOUVEAU/Plugins/SerialCOM/" 2>/dev/null && echo "  ok  SERIALCOM.uplugin"
copier "Plugins/SerialCOM/Source" "Plugins/SerialCOM/Source"

echo ""
echo "=== 2bis/4  Plugins audio — capture micro et VAD ==="
echo "  (SileroVAD borne les segments de parole : c'est lui qui dit au"
echo "   sidecar quand le visiteur a fini de parler)"
for p in RuntimeAudioImporter RuntimeAudioImporterSileroVAD; do
  mkdir -p "$NOUVEAU/Plugins/$p"
  cp "$ANCIEN/Plugins/$p/$p.uplugin" "$NOUVEAU/Plugins/$p/" 2>/dev/null && echo "  ok  $p.uplugin"
  copier "Plugins/$p/Source"    "Plugins/$p/Source"
  copier "Plugins/$p/Content"   "Plugins/$p/Content"
  copier "Plugins/$p/Resources" "Plugins/$p/Resources"
done

echo ""
echo "=== 3/4  MetaHuman ==="
copier "Content/MetaHumans/AgentGermain" "Content/MetaHumans/AgentGermain"
copier "Content/MetaHumans/Common"       "Content/MetaHumans/Common"
# L'uniforme est reference par BP_AgentGermain (ceinture, bottes, casquette,
# pantalon, chemise), pas par la map. Le premier tri, base sur les seules
# references de Studio.umap, l'avait donc ecarte a tort : l'agent
# apparaissait nu.
copier "Content/PoliceUniform"           "Content/PoliceUniform"

echo ""
echo "=== 4/4  Scenographie et logique existante ==="
for d in GlitchFx StampFx Materials Meshes Lidar FirstPerson; do
  copier "Content/$d" "Content/$d"
done
copier "Content/Studio.umap"                "Content/Studio.umap"
copier "Content/BP_ConvaiCharacterBase.uasset" "Content/BP_ConvaiCharacterBase.uasset"
copier "Content/BP_AutoGainComponent.uasset"   "Content/BP_AutoGainComponent.uasset"

echo ""
echo "=== 5/5  Contournement du build Convai ==="
cat <<'POURQUOI'
  UBT ne planifie pas le module runtime Convai, alors qu'il en reclame la
  bibliotheque a l'edition de liens. Cause non identifiee apres huit
  tentatives. On fournit donc a la main ce qu'il refuse de produire, en
  le reprenant de l'archive — ou le module avait ete compile pour la 5.7.

  On ne reprend QUE ces fichiers. Un Intermediate complet n'est pas
  transportable : ses .rsp portent des chemins absolus vers l'ancien
  emplacement du projet.
POURQUOI

LIB="Intermediate/Build/Win64/x64/UnrealEditor/Development/Convai"
INC="Intermediate/Build/Win64/UnrealEditor/Inc/Convai"

# En-tetes generes par UHT, que UHT ne regenere jamais pour ce module
copier "Plugins/Convai/$INC" "Plugins/Convai/$INC"

# DLL et bibliotheque d'import (les .pdb, 148 Mo de symboles, sont inutiles)
mkdir -p "$NOUVEAU/Plugins/Convai/Binaries/Win64" "$NOUVEAU/Plugins/Convai/$LIB"
for f in UnrealEditor-Convai.dll UnrealEditor-ConvaiEditor.dll UnrealEditor.modules; do
  src="$ANCIEN/Plugins/Convai/Binaries/Win64/$f"
  [ -f "$src" ] && cp "$src" "$NOUVEAU/Plugins/Convai/Binaries/Win64/" && echo "  ok  $f"
done
for e in lib exp; do
  src="$ANCIEN/Plugins/Convai/$LIB/UnrealEditor-Convai.$e"
  [ -f "$src" ] && cp "$src" "$NOUVEAU/Plugins/Convai/$LIB/" && echo "  ok  UnrealEditor-Convai.$e"
done

echo ""
echo "=== NON MIGRE (volontairement) ==="
cat <<'NOTE'
  AgentGrondin, AgentSmith   aucun Character ID, aucune reference
  PoliceUniform              non reference par la scene
  Maps/MHC_LightingPresets   18 maps d'exemple MetaHuman
  Intermediate, Binaries     regeneres a la compilation
  Saved, DerivedDataCache    caches
NOTE

echo ""
echo "=== BILAN ==="
du -sm "$NOUVEAU/Content" "$NOUVEAU/Plugins" 2>/dev/null
echo "  --- disque ---"
df -h /c | tail -1
