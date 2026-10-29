#!/usr/bin/env bash
# Runs the hook test under Wine against the real game exe. Needs a build
# with -DFH2MOD_TESTS=ON in build-test/:
#   tests/run_hook_test.sh <forza_durango_release_final.exe> [another game's exe]
# With another Forza game's exe (Forza Motorsport 5's, say), two more launches
# check that the platform leaves that game alone (Azimuth 1.0.1 runs both).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
b=$root/build-test
exe=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
other=
[ -n "${2:-}" ] && other=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
phases="plain eo wine-builtin changed-code crash"
[ -n "$other" ] && phases="$phases other-game skip-launcher"
status=0
for phase in $phases; do
    dir=$(mktemp -d)
    mkdir -p "$dir/fh2-mods" "$dir/FH2/Mount/media/Stripped/StringTables"
    cp "$b/hook_test.exe" "$dir/azimuth.exe"
    cp "$b/d3dcompiler_47.dll" "$dir/"
    cp "$b/HookMod.dll" "$b/Legacy.dll" "$dir/fh2-mods/"
    ln -s "$exe" "$dir/FH2/Mount/forza_durango_release_final.exe"
    mapped=$exe
    args=()
    if [ "$phase" = other-game ] || [ "$phase" = skip-launcher ]; then
        # Azimuth 1.0.1's settings, with Forza Motorsport 5 next to FH2.
        mkdir -p "$dir/data" "$dir/FM5/Mount"
        ln -s "$other" "$dir/FM5/Mount/forza_durango_release_final.exe"
        printf '[launcher]\r\ngame=%s\r\n[title-1a96545b]\r\nfolder=FH2\r\nname=Forza Horizon 2\r\n[title-7b35d917]\r\nfolder=FM5\r\nname=Forza Motorsport 5\r\n' \
            "$([ "$phase" = skip-launcher ] && echo 7b35d917 || echo 1a96545b)" > "$dir/data/azimuth.ini"
        mapped=$other
        [ "$phase" = skip-launcher ] && args=(--skip-launcher)
    fi
    overrides=
    if [ "$phase" = eo ] || [ "$phase" = wine-builtin ]; then
        cp "$b/winmm.dll" "$dir/"
        # As on Linux: Extra Options' winmm.dll loads only with this override. Without it (the
        # wine-builtin phase), Wine uses its own winmm.dll and reports the game folder's path for it.
        [ "$phase" = eo ] && overrides="winmm=n,b"
        # Last launch's log says it's done; this launch's mustn't be taken from it.
        printf 'AZIMUTH: winmm proxy initialized; original TLS helper retained.\r\n' > "$dir/FH2_ExtraOptions.log"
        touch -d '1 hour ago' "$dir/FH2_ExtraOptions.log"
    fi
    echo "==== $phase"
    (cd "$dir" && WINEDLLOVERRIDES="$overrides" wine azimuth.exe "$phase" "$(winepath -w "$mapped" 2>/dev/null)" "${args[@]}") || status=1
    rm -rf "$dir"
done
exit $status
