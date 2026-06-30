#!/usr/bin/env bash
# Compile an Abyss program to a native Android binary and run it on a connected
# device or emulator.
#
#   abyssc --emit-c  ->  NDK clang (ABI-matched, 16 KB-page aligned)
#                    ->  adb push  ->  adb shell run
#
# Runs an Abyss program as a native *console* binary on Android (the
# component/state/render runtime renders to a text widget tree). A graphical
# Skia surface and an APK wrapper are later milestones; this proves the
# compile-and-run-on-device path end to end.
#
# Usage: scripts/android_run.sh path/to/program.aby [path/to/abyssc]
set -euo pipefail

SRC="${1:?usage: android_run.sh <file.aby> [abyssc]}"
ABYSSC="${2:-./abyssc}"
[ -x "$ABYSSC" ] || ABYSSC="./abyssc.exe"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
[ -d "$SDK" ] || SDK="$HOME/Android/Sdk"
[ -d "$SDK" ] || { echo "Android SDK not found; set ANDROID_HOME" >&2; exit 1; }

ADB="$SDK/platform-tools/adb"
NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort | tail -1)"
[ -n "$NDK" ] || { echo "No NDK under $SDK/ndk" >&2; exit 1; }
# host tag: linux-x86_64 / darwin-x86_64
HOST="$(uname | tr '[:upper:]' '[:lower:]')-x86_64"
CLANG="$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang"

ABI="$("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r')"
case "$ABI" in
  x86_64)     TARGET=x86_64-linux-android21 ;;
  arm64-v8a)  TARGET=aarch64-linux-android21 ;;
  x86)        TARGET=i686-linux-android21 ;;
  armeabi*)   TARGET=armv7a-linux-androideabi21 ;;
  *) echo "Unsupported ABI: $ABI" >&2; exit 1 ;;
esac
echo "device ABI = $ABI  ->  target = $TARGET"

BASE="$(basename "${SRC%.*}")"
CFILE="${TMPDIR:-/tmp}/$BASE.android.c"
BIN="${TMPDIR:-/tmp}/$BASE.android"

"$ABYSSC" --emit-c "$SRC" > "$CFILE"
# -Wl,-z,max-page-size=16384 is REQUIRED on recent Android (16 KB pages);
# a 4 KB-aligned binary SIGSEGVs at load.
"$CLANG" --target="$TARGET" -O2 -Wl,-z,max-page-size=16384 "$CFILE" -o "$BIN"

"$ADB" push "$BIN" "/data/local/tmp/$BASE" >/dev/null
"$ADB" shell chmod 755 "/data/local/tmp/$BASE"
echo "=== running $BASE on $ABI ==="
exec "$ADB" shell "/data/local/tmp/$BASE"
