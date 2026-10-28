#!/usr/bin/env bash
# Cross-compiles the FH2 Mod Platform with llvm-mingw and stages dist/:
#   dist/d3dcompiler_47.dll      the platform (next to azimuth.exe)
#   dist/DWrite.dll              the same file, for Azimuth 1.0.1 and later
#   dist/examples/HelloMod.dll   the example mod (not for players)
#   dist/include/fh2mod.h        the API header
#   dist/include/fh2game.h       names for the game addresses mods use
#   dist/README.txt, dist/LICENSE.txt, dist/LICENSE-MinHook.txt
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
for d in /opt/llvm-mingw/bin /usr/local/llvm-mingw/bin; do [ -d "$d" ] && PATH=$d:$PATH; done
cmake -S "$root" -B "$root/build" -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-w64-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" -j "${JOBS:-$(nproc)}"
rm -rf "$root/dist"
mkdir -p "$root/dist/examples" "$root/dist/include"
cp "$root/build/d3dcompiler_47.dll" "$root/build/DWrite.dll" "$root/dist/"
cp "$root/build/HelloMod.dll" "$root/dist/examples/"
cp "$root/include/fh2mod.h" "$root/include/fh2game.h" "$root/dist/include/"
cp "$root/scripts/dist-readme.txt" "$root/dist/README.txt"
cp "$root/LICENSE" "$root/dist/LICENSE.txt"
cp "$root/third_party/minhook/LICENSE.txt" "$root/dist/LICENSE-MinHook.txt"
(cd "$root/dist" && find . -type f -printf '  %P\n')
