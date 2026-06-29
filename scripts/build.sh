#!/usr/bin/env sh
# Build abyssc on macOS / Linux without `make`.
#   ./scripts/build.sh   ->  ./abyssc
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
command -v clang >/dev/null 2>&1 && [ -z "${CC_SET:-}" ] && CC=clang
echo "Building abyssc with $CC ..."
"$CC" -std=c11 -Wall -Wextra -O2 src/*.c -o abyssc
echo "Done. Try: ./abyssc examples/run_demo.aby"
