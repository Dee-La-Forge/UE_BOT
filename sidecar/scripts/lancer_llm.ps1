# Lance llama.cpp server sur la boucle locale.
#
#   .\scripts\lancer_llm.ps1
#   .\scripts\lancer_llm.ps1 -Modele qwen2.5-3b-instruct-q4_k_m.gguf
#
# Processus separe, par choix d'architecture : si le modele plante, le
# sidecar et Unreal survivent, et on redemarre le LLM seul.

param(
    # Le 3B : c'est le seul present dans models/llm/. Le defaut pointait
    # vers un 7B jamais telecharge — et l'etiquette de config.yaml, elle,
    # disait 3B : les mesures s'attribuaient au mauvais modele. Si la
    # qualite de dialogue reste insuffisante apres les reglages de prompt
    # et de grammaire, telecharger le 7B est le levier suivant (~150 ms de
    # plus au premier token, mesure).
    [string]$Modele = "qwen2.5-3b-instruct-q4_k_m.gguf",
    [int]$Port = 8080,
    [int]$Contexte = 4096,
    [int]$CouchesGpu = 99   # 99 = tout le modele sur GPU
)

$ErrorActionPreference = "Stop"
$racine = Split-Path -Parent $PSScriptRoot

$serveur = Join-Path $racine "tools\llama.cpp\llama-server.exe"
$poids   = Join-Path $racine "models\llm\$Modele"

if (-not (Test-Path $serveur)) {
    Write-Host "llama-server.exe introuvable : $serveur" -ForegroundColor Red
    Write-Host "Voir scripts\setup.ps1, etape 4." -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path $poids)) {
    Write-Host "Modele introuvable : $poids" -ForegroundColor Red
    Write-Host "Voir scripts\setup.ps1, etape 3." -ForegroundColor Yellow
    exit 1
}

Write-Host "`n  Modele   : $Modele" -ForegroundColor Cyan
Write-Host "  Ecoute   : http://127.0.0.1:$Port" -ForegroundColor Cyan
Write-Host "  Contexte : $Contexte tokens`n" -ForegroundColor Cyan

& $serveur `
    --model $poids `
    --host 127.0.0.1 `
    --port $Port `
    --ctx-size $Contexte `
    --n-gpu-layers $CouchesGpu `
    --flash-attn `
    --cont-batching
