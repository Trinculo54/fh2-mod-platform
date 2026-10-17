#!/usr/bin/env bash
# Runs the hook test under Wine against the real game exe. Needs a build
# with -DFH2MOD_TESTS=ON in build-test/:
#   tests/run_hook_test.sh <forza_durango_release_final.exe>
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
exe=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
status=0
for phase in plain eo crash; do
    dir=$(mktemp -d)
    mkdir -p "$dir/fh2-mods" "$dir/FH2/Mount/media/Stripped/StringTables"
    cp "$b/hook_test.exe" "$dir/azimuth.exe"
    cp "$b/d3dcompiler_47.dll" "$dir/"
    cp "$b/HookMod.dll" "$dir/fh2-mods/"
    ln -s "$exe" "$dir/FH2/Mount/forza_durango_release_final.exe"
    if [ "$phase" = eo ]; then
        cp "$b/winmm.dll" "$dir/"
        # Last launch's log says it's done; this launch's mustn't be taken from it.
        printf 'AZIMUTH: winmm proxy initialized; original TLS helper retained.\r\n' > "$dir/FH2_ExtraOptions.log"
        touch -d '1 hour ago' "$dir/FH2_ExtraOptions.log"
    fi
    echo "==== $phase"
    (cd "$dir" && wine azimuth.exe "$phase" "$(winepath -w "$exe" 2>/dev/null)") || status=1
    rm -rf "$dir"
done
exit $status
