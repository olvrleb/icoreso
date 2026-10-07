#!/usr/bin/env bash
# Cross-compiles the mod's test harness with MinGW and runs it under Wine.
# Requires: x86_64-w64-mingw32-g++, wine, python3 + Pillow (xvfb-run optional).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="${BUILD_DIR:-$(mktemp -d)}"
mkdir -p "$build/out"

python3 "$here/make_test_icon.py" "$build/test.ico"
cp "$here/test_app.rc" "$build/app.rc"
(cd "$build" &&
    x86_64-w64-mingw32-windres app.rc -O coff -o app.res &&
    echo 'int main(){return 0;}' > app.c &&
    x86_64-w64-mingw32-gcc app.c app.res -o app.exe)

x86_64-w64-mingw32-g++ -municode -std=c++20 -O2 -Wall -Wextra \
    -include "$here/windhawk_stub.h" "$here/render_test.cpp" \
    -o "$build/render_test.exe" -lgdi32 -luser32 -lshell32 -lole32 -lwindowscodecs -static

wine_bin="$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)"
runner=()
command -v xvfb-run >/dev/null && runner=(xvfb-run -a)
win() { echo "Z:${1//\//\\}"; }
WINEDEBUG=-all "${runner[@]}" "$wine_bin" "$build/render_test.exe" \
    "$(win "$build/test.ico")" "$(win "$build/app.exe")" "$(win "$build/out")"
