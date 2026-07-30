# Installation du sidecar — environnement Python UNIQUEMENT.
#
#   .\scripts\setup.ps1
#
# Les modeles (voix Piper, LLM GGUF) et llama.cpp sont provisionnes par
# scripts/telecharger.sh, qui epingle les versions et verifie les tailles :
# c'est la voie unique. Ce script ne fait plus que le venv + pip — il
# annoncait avant un 7B « par defaut » et un telechargement manuel, en
# contradiction avec telecharger.sh et le 3B reellement mesure.

$ErrorActionPreference = "Stop"
$racine = Split-Path -Parent $PSScriptRoot

Write-Host "`n=== Sidecar IA locale — environnement Python ===`n" -ForegroundColor Cyan

$venv = Join-Path $racine ".venv"
if (-not (Test-Path $venv)) {
    Write-Host "[1/2] Creation de l'environnement Python 3.12..." -ForegroundColor Yellow
    & py -3.12 -m venv $venv
} else {
    Write-Host "[1/2] Environnement Python deja present." -ForegroundColor Green
}

$python = Join-Path $venv "Scripts\python.exe"
& $python -m pip install --quiet --upgrade pip
& $python -m pip install --quiet -r (Join-Path $racine "requirements.txt")
Write-Host "[2/2] Dependances installees." -ForegroundColor Green

Write-Host "`n=== Environnement pret ===`n" -ForegroundColor Cyan
Write-Host "  Modeles + llama.cpp :  ./scripts/telecharger.sh   (Git Bash)" -ForegroundColor White
Write-Host "  Puis                :  .\scripts\lancer_llm.ps1" -ForegroundColor White
Write-Host "  Et                  :  .venv\Scripts\python.exe main.py`n" -ForegroundColor White
