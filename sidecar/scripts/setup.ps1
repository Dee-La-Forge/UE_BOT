# Installation du sidecar IA locale
#
#   .\scripts\setup.ps1
#
# Telecharge les modeles et prepare l'environnement Python.
# Les poids ne sont PAS versionnes (voir .gitignore) : ~10 Go.

$ErrorActionPreference = "Stop"
$racine = Split-Path -Parent $PSScriptRoot

Write-Host "`n=== Sidecar IA locale — installation ===`n" -ForegroundColor Cyan

# --- 1. Environnement Python -------------------------------------------
$venv = Join-Path $racine ".venv"
if (-not (Test-Path $venv)) {
    Write-Host "[1/4] Creation de l'environnement Python 3.12..." -ForegroundColor Yellow
    & py -3.12 -m venv $venv
} else {
    Write-Host "[1/4] Environnement Python deja present." -ForegroundColor Green
}

$python = Join-Path $venv "Scripts\python.exe"
& $python -m pip install --quiet --upgrade pip
& $python -m pip install --quiet -r (Join-Path $racine "requirements.txt")
Write-Host "      Dependances installees." -ForegroundColor Green

# --- 2. Voix Piper (francais) ------------------------------------------
$dossierPiper = Join-Path $racine "models\piper"
New-Item -ItemType Directory -Force -Path $dossierPiper | Out-Null

$voix = "fr_FR-siwis-medium"
$baseUrl = "https://huggingface.co/rhasspy/piper-voices/resolve/main/fr/fr_FR/siwis/medium"

foreach ($ext in @("onnx", "onnx.json")) {
    $cible = Join-Path $dossierPiper "$voix.$ext"
    if (-not (Test-Path $cible)) {
        Write-Host "[2/4] Telechargement de la voix $voix.$ext..." -ForegroundColor Yellow
        Invoke-WebRequest -Uri "$baseUrl/$voix.$ext" -OutFile $cible
    }
}
Write-Host "      Voix Piper prete." -ForegroundColor Green

# --- 3. LLM (GGUF) ------------------------------------------------------
# Telechargement manuel : ~4,7 Go, mieux vaut le maitriser.
$dossierLlm = Join-Path $racine "models\llm"
New-Item -ItemType Directory -Force -Path $dossierLlm | Out-Null

Write-Host "[3/4] LLM — telechargement manuel requis." -ForegroundColor Yellow
Write-Host @"
      Deposer dans models\llm\ :

        qwen2.5-7b-instruct-q4_k_m.gguf   (~4,7 Go)  <- defaut
        qwen2.5-3b-instruct-q4_k_m.gguf   (~2,0 Go)  <- comparatif latence

      Source : https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF
"@ -ForegroundColor Gray

# --- 4. llama.cpp server ------------------------------------------------
Write-Host "[4/4] llama.cpp server — binaire CUDA precompile." -ForegroundColor Yellow
Write-Host @"
      Recuperer la release Windows CUDA :
        https://github.com/ggerganov/llama.cpp/releases

      Extraire dans  tools\llama.cpp\  puis lancer  scripts\lancer_llm.ps1

      Le LLM tourne dans un processus separe, par choix : si le modele
      plante, le sidecar et Unreal survivent.
"@ -ForegroundColor Gray

# --- NeuroSync ----------------------------------------------------------
Write-Host "`n--- NeuroSync (lipsync) ---" -ForegroundColor Cyan
Write-Host @"
      Local API : https://huggingface.co/convaitech/NEUROSYNC
      Sortie    : 61 blendshapes ARKit + 7 emotions, 60 fps
      Livraison : LiveLink — deja active dans le projet Unreal

      LICENCE : gratuit sous 1 M`$ de CA annuel, licence commerciale
      au-dela. A clarifier pour une installation museale.
"@ -ForegroundColor Gray

Write-Host "`n=== Installation terminee ===`n" -ForegroundColor Cyan
Write-Host "  Etape suivante :  .\scripts\lancer_llm.ps1" -ForegroundColor White
Write-Host "  Puis          :  .venv\Scripts\python.exe -m bench.bench_pipeline --tours 5`n" -ForegroundColor White
