// What's different under Wine and Proton (src/wine.cpp), with no game. Exits 0 when every check passes.
#include "wine.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace fh2platform;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

constexpr uint64_t kBase = 0x140000000ull, kSize = 0x3A18000ull;

} // namespace

int main() {
    // The folder volume's vtable (game+0x2EAB918): two pointers into the game's code. On disk they're
    // 0x1411E5B30 and 0x14021DEE0; Proton has mapped the image at 0x18940000, below its preferred base.
    check(same_pointer(0x1411E5B30ull, 0x1411E5B30ull, kBase, kBase, kSize),
          "mapped at its preferred base: the same pointer matches (Windows)");
    check(!same_pointer(0x1411E5B30ull, 0x1411E5B31ull, kBase, kBase, kSize),
          "mapped at its preferred base: a different pointer doesn't");
    check(same_pointer(0x1411E5B30ull, 0x19B25B30ull, kBase, 0x18940000ull, kSize),
          "mapped below it (0x18940000): the pointer moved with the image matches");
    check(same_pointer(0x14021DEE0ull, 0x18B5DEE0ull, kBase, 0x18940000ull, kSize),
          "mapped below it: the vtable's second pointer too");
    check(!same_pointer(0x1411E5B30ull, 0x1411E5B30ull, kBase, 0x18940000ull, kSize),
          "mapped below it: the pointer as on disk doesn't (the bytes the old check compared)");
    check(same_pointer(0x1411E5B30ull, 0x1C11E5B30ull, kBase, 0x1C0000000ull, kSize),
          "mapped above it (0x1C0000000): the pointer moved with the image matches");
    check(!same_pointer(0x1411E5B30ull, 0x1411E5B30ull + 0x1000, kBase, 0x18940000ull, kSize),
          "mapped elsewhere: a pointer moved by some other amount doesn't");
    check(!same_pointer(0x0ull, 0x0ull - kBase + 0x18940000ull, kBase, 0x18940000ull, kSize),
          "a value that isn't a pointer into the image never matches, even shifted");
    check(!same_pointer(kBase + kSize, kBase + kSize, kBase, kBase, kSize),
          "the first address past the image isn't in it");
    check(same_pointer(kBase + kSize - 1, kBase + kSize - 1, kBase, kBase, kSize),
          "the image's last byte is in it");
    check(!same_pointer(kBase - 1, kBase - 1, kBase, kBase, kSize), "the address before the image isn't in it");

    // Load orders, as Wine reads them (WINEDLLOVERRIDES values and winecfg's registry values).
    check(parse_load_order("n,b") == LoadOrder::native_first, "\"n,b\" is native first");
    check(parse_load_order("native,builtin") == LoadOrder::native_first, "\"native,builtin\" (winecfg) is native first");
    check(parse_load_order("n") == LoadOrder::native_first, "\"n\" is native first");
    check(parse_load_order("N, B") == LoadOrder::native_first, "capitals and spaces are fine");
    check(parse_load_order("b,n") == LoadOrder::builtin_first, "\"b,n\" is builtin first");
    check(parse_load_order("builtin") == LoadOrder::builtin_first, "\"builtin\" is builtin first");
    check(parse_load_order("") == LoadOrder::disabled, "nothing is disabled");
    check(parse_load_order(" , ") == LoadOrder::disabled, "only separators is disabled");
    check(parse_load_order("x,n") == LoadOrder::native_first, "a word that's neither is skipped");

    // WINEDLLOVERRIDES
    check(env_load_order("winmm=n,b", "winmm") == LoadOrder::native_first, "winmm=n,b (docs/linux.md)");
    check(env_load_order("winmm,d3dcompiler_47=n,b", "winmm") == LoadOrder::native_first,
          "winmm first of two names (docs/linux.md, step 2)");
    check(env_load_order("winmm,dwrite=n,b", "winmm") == LoadOrder::native_first, "winmm with dwrite (Azimuth 1.0.3)");
    check(env_load_order("dxgi,winmm=n,b", "winmm") == LoadOrder::native_first, "winmm second of two names");
    check(env_load_order("WinMM=n,b", "winmm") == LoadOrder::native_first, "names without regard to case");
    check(env_load_order("steam.exe=b;dotnetfx35.exe=b;d3d11=n;dxgi=n;winmm=n,b", "winmm") == LoadOrder::native_first,
          "winmm after Proton's own entries");
    check(env_load_order("steam.exe=b;d3d11=n;dxgi=n", "winmm") == LoadOrder::unsaid,
          "Proton's own entries alone say nothing about winmm");
    check(env_load_order("", "winmm") == LoadOrder::unsaid, "no WINEDLLOVERRIDES says nothing");
    check(env_load_order("winmm=b,n", "winmm") == LoadOrder::builtin_first, "winmm=b,n is builtin first");
    check(env_load_order("winmm=", "winmm") == LoadOrder::disabled, "winmm= is disabled");
    check(env_load_order("winmm=n,b;winmm=b", "winmm") == LoadOrder::builtin_first, "the later entry counts");
    check(env_load_order("winmmx=n;xwinmm=n", "winmm") == LoadOrder::unsaid, "only the whole name counts");
    check(env_load_order("winmm.dll=n,b", "winmm") == LoadOrder::unsaid,
          "winmm.dll isn't winmm (Wine looks the name up without .dll)");
    check(env_load_order("*winmm=n,b", "*winmm") == LoadOrder::native_first, "*winmm, the other name Wine tries");
    check(env_load_order("winmm", "winmm") == LoadOrder::unsaid, "an entry without = is skipped");
    check(env_load_order(";;winmm=n,b;", "winmm") == LoadOrder::native_first, "empty entries are skipped");
    check(env_load_order("d3d11=n;winmm=n,b", "d3d11") == LoadOrder::native_first, "other names are read too");

    // The names Wine looks the winmm.dll next to azimuth.exe up by, first to last.
    const std::vector<std::string> names = wine_override_names("Z:\\games\\Azimuth\\winmm.dll");
    check(names == std::vector<std::string>{"Z:\\games\\Azimuth\\winmm", "*winmm", "winmm"},
          "the full path without .dll, then *winmm, then winmm");
    check(wine_override_names("C:\\Azimuth\\WINMM.DLL") ==
              std::vector<std::string>{"C:\\Azimuth\\WINMM", "*WINMM", "WINMM"},
          ".DLL in capitals goes too");
    check(wine_override_names("winmm.dll") == std::vector<std::string>{"*winmm", "winmm"}, "no path, no full name");

    // Which source counts, as Wine decides: each name in turn, and for each WINEDLLOVERRIDES, then
    // winecfg's settings for azimuth.exe, then its settings for every program.
    struct Fake {
        std::map<std::string, LoadOrder> says;
        int asked = 0;
        LoadOrder operator()(const std::string& name) {
            ++asked;
            const auto it = says.find(name);
            return it == says.end() ? LoadOrder::unsaid : it->second;
        }
    };
    auto decide = [&](Fake& env, Fake& app, Fake& global) {
        LoadOrderSources from;
        from.env    = [&](const std::string& n) { return env(n); };
        from.app    = [&](const std::string& n) { return app(n); };
        from.global = [&](const std::string& n) { return global(n); };
        return wine_load_order(names, from);
    };
    {
        Fake env, app, global;
        check(decide(env, app, global) == LoadOrder::unsaid, "nothing said anywhere is unsaid (Wine's own winmm.dll)");
        check(env.asked == 3 && app.asked == 3 && global.asked == 3, "every source is asked about every name");
    }
    {
        Fake env{{{"winmm", LoadOrder::native_first}}}, app{{{"winmm", LoadOrder::builtin_first}}}, global;
        check(decide(env, app, global) == LoadOrder::native_first, "WINEDLLOVERRIDES before azimuth.exe's settings");
    }
    {
        Fake env, app{{{"winmm", LoadOrder::builtin_first}}}, global{{{"winmm", LoadOrder::native_first}}};
        check(decide(env, app, global) == LoadOrder::builtin_first, "azimuth.exe's settings before everyone's");
    }
    {
        Fake env, app, global{{{"winmm", LoadOrder::native_first}}};
        check(decide(env, app, global) == LoadOrder::native_first, "everyone's settings when nothing else says");
    }
    {
        Fake env{{{"winmm", LoadOrder::builtin_first}}}, app, global{{{"*winmm", LoadOrder::native_first}}};
        check(decide(env, app, global) == LoadOrder::native_first,
              "*winmm anywhere before winmm, even winmm in WINEDLLOVERRIDES");
    }
    {
        Fake env{{{"*winmm", LoadOrder::builtin_first}}},
            app{{{"Z:\\games\\Azimuth\\winmm", LoadOrder::native_first}}}, global;
        check(decide(env, app, global) == LoadOrder::native_first, "the full path before *winmm");
        check(env.asked == 1 && app.asked == 1 && global.asked == 0, "the first answer ends the search");
    }
    {
        Fake env{{{"winmm", LoadOrder::disabled}}}, app, global{{{"winmm", LoadOrder::native_first}}};
        check(decide(env, app, global) == LoadOrder::disabled, "disabled is an answer too");
    }
    {
        LoadOrderSources from;
        from.env = [](const std::string& n) { return n == "winmm" ? LoadOrder::native_first : LoadOrder::unsaid; };
        check(wine_load_order(names, from) == LoadOrder::native_first, "sources that aren't given are skipped");
    }

    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
