<#
.SYNOPSIS
    Compile an Abyss program to a native Android binary and run it on a
    connected device or emulator.

.DESCRIPTION
    Pipeline:  abyssc --emit-c  ->  NDK clang (ABI-matched, 16 KB-page aligned)
               ->  adb push  ->  adb shell run.

    This runs an Abyss program as a native *console* binary on Android (the
    component/state/render runtime renders to a text widget tree). A graphical
    Skia surface and an APK wrapper are later milestones; this proves the
    compile-and-run-on-device path end to end.

.EXAMPLE
    pwsh scripts/android_run.ps1 examples/counter_app.aby
#>
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Abyssc = ".\abyssc.exe"
)
$ErrorActionPreference = "Stop"

$sdk = $env:ANDROID_HOME
if (-not $sdk) { $sdk = $env:ANDROID_SDK_ROOT }
if (-not $sdk) { $sdk = "$env:LOCALAPPDATA\Android\Sdk" }
if (-not (Test-Path $sdk)) { throw "Android SDK not found. Set ANDROID_HOME." }

$adb = Join-Path $sdk "platform-tools\adb.exe"
$ndkRoot = Join-Path $sdk "ndk"
$ndk = Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $ndk) { throw "No NDK found under $ndkRoot. Install one via the SDK Manager." }
$clang = Join-Path $ndk.FullName "toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"

# Match the device ABI (an emulator is usually x86_64; a phone is arm64-v8a).
$abi = (& $adb shell getprop ro.product.cpu.abi).Trim()
switch -Wildcard ($abi) {
    "x86_64"    { $target = "x86_64-linux-android21" }
    "arm64-v8a" { $target = "aarch64-linux-android21" }
    "x86"       { $target = "i686-linux-android21" }
    "armeabi*"  { $target = "armv7a-linux-androideabi21" }
    default     { throw "Unsupported ABI: $abi" }
}
Write-Host "device ABI = $abi  ->  target = $target"

$base  = [IO.Path]::GetFileNameWithoutExtension($Source)
$cfile = Join-Path $env:TEMP "$base.android.c"
$bin   = Join-Path $env:TEMP "$base.android"

# 1. Abyss -> C
& $Abyssc --emit-c $Source | Set-Content -Encoding ascii $cfile
if ($LASTEXITCODE -ne 0) { throw "abyssc --emit-c failed" }

# 2. C -> native Android binary.
#    -Wl,-z,max-page-size=16384 is REQUIRED: recent Android images use 16 KB
#    memory pages, and a 4 KB-aligned binary SIGSEGVs at load.
& $clang "--target=$target" -O2 "-Wl,-z,max-page-size=16384" $cfile -o $bin
if ($LASTEXITCODE -ne 0) { throw "NDK clang failed" }

# 3. push + run
& $adb push $bin "/data/local/tmp/$base" | Out-Null
& $adb shell chmod 755 "/data/local/tmp/$base"
Write-Host "=== running $base on $abi ==="
& $adb shell "/data/local/tmp/$base"
exit $LASTEXITCODE
