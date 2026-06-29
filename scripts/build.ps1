# Build abyssc on Windows (no `make` needed).
#   pwsh scripts/build.ps1   ->  .\abyssc.exe
#
# Requires LLVM/clang. The generated C the backend emits uses GNU
# statement-expressions and _Generic, so clang (or gcc) is needed — not MSVC.
#   winget install LLVM.LLVM      (or)    choco install llvm
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
    Write-Error "clang not found. Install LLVM: 'winget install LLVM.LLVM' or 'choco install llvm'."
}

$src = Get-ChildItem -Path src -Filter *.c | ForEach-Object { $_.FullName }
Write-Host "Building abyssc.exe with clang ..."
& clang -std=c11 -Wall -Wextra -O2 $src -o abyssc.exe
Write-Host "Done. Try: .\abyssc.exe examples\run_demo.aby"
