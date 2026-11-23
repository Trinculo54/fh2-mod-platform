// Offline test of the pad layer preview against the real game exe, run as "azimuth.exe" by
// tests/run_pad_test.sh:
//   azimuth.exe d3dcompiler_47.dll fh2-mods\PadMod.dll
//   FH2\Mount\forza_durango_release_final.exe
//   azimuth.exe <path to forza_durango_release_final.exe>
// The exe is mapped as in hook_test.cpp. Once the platform says pad layers are on, the test makes a
// controller manager and a slot of its own, points the game's manager pointer at them, and calls the
// game's own "release taken controls" function (game+0x10768D0), which the platform has hooked, the
// way the game's pad update does: once per frame, after setting the slot's held controls. So the game's
// code and the platform's detour both run, and the slot's mask of taken controls says what the game's
// actions would see.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(x) do { bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

static std::string slurp(const fs::path& f) {
    std::ifstream in{f, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), {}};
}
static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }

// As hook_test.cpp: sections at their addresses, code read/execute, headers last.
static char* map_game(const wchar_t* path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> file((std::istreambuf_iterator<char>(in)), {});
    if (file.size() < 4096) return nullptr;
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    auto* base = static_cast<char*>(VirtualAlloc(reinterpret_cast<void*>(nt->OptionalHeader.ImageBase),
                                                 nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) return nullptr;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        std::memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData,
                    std::min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD c = sec[i].Characteristics;
        DWORD prot    = c & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ : c & IMAGE_SCN_MEM_WRITE ? PAGE_READWRITE : PAGE_READONLY;
        DWORD old     = 0;
        VirtualProtect(base + sec[i].VirtualAddress, std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData), prot, &old);
    }
    // What Extra Options would do (hook_test.cpp): the platform waits for no Extra Options here.
    DWORD old = 0;
    VirtualProtect(base + 0x199FBE0, 1, PAGE_EXECUTE_READWRITE, &old);
    base[0x199FBE0] = static_cast<char>(0xCC);
    VirtualProtect(base + 0x199FBE0, 1, old, &old);
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    VirtualProtect(base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    return base;
}

static uint64_t C(int control) { return 1ull << control; }

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: azimuth.exe <forza_durango_release_final.exe>\n");
        return 2;
    }
    CHECK(LoadLibraryW(L"d3dcompiler_47.dll") != nullptr);
    Sleep(500);
    char* game = map_game(argv[1]);
    CHECK(game != nullptr);   // "press Play"
    std::string log;
    // the pad layer, and the pop-ups' hooks (API 10), which start in every session though PadMod never declared any
    for (int i = 0; i < 300; ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
        if ((has(log, "pad layers are on") || has(log, "pad layers are off")) && has(log, "[Pop-ups] pop-ups and tips are ")) break;
    }
    CHECK(has(log, "[Pop-ups] pop-ups and tips are ready"));
    CHECK(has(log, "[PadMod] pad layer Chat: while LB is held, the game doesn't see Up+Down+Left+Right"));
    CHECK(has(log, "[PadMod] layer Chat added"));
    CHECK(has(log, "[PadMod] add_layer: pad layer Both clashes with PadMod's Chat (LB with Up+Down+Left+Right)"));
    CHECK(has(log, "[PadMod] layer Both refused"));
    CHECK(has(log, "[Pad layer] pad layers are on (4)"));
    CHECK(has(log, "[PadMod] pad layer Editor: latched: while its mod has it open, the game doesn't see X") && has(log, "[PadMod] layer Editor added"));
    CHECK(has(log, "[PadMod] pad layer Flight: latched: while its mod has it open, the game doesn't see Y; in the driving view only"));
    CHECK(has(log, "[PadMod] add_layer: pad layer Held is latched (FH2_PAD_LAYER_LATCH), so it can't have a modifier too") && has(log, "[PadMod] layer Held refused"));
    CHECK(has(log, "[PadMod] set_open: there's no latched pad layer called Chat") && has(log, "[PadMod] set_open Chat: 0"));
    CHECK(has(log, "[PadMod] set_modifier: pad layer Editor is latched") && has(log, "[PadMod] set_modifier Editor: 0"));
    CHECK(has(log, "[PadMod] pad interface version 2"));
    CHECK(has(log, "[PadMod] pad layer Drive: while RB is held, the game doesn't see A; in the driving view only") && has(log, "[PadMod] layer Drive added"));
    CHECK(has(log, "[PadMod] add_layer: pad layer Odd has flags this platform doesn't know") && has(log, "[PadMod] layer Odd refused"));
    if (!game || !has(log, "pad layers are on")) {
        std::printf("---- platform.log\n%s----\n", log.c_str());
        std::printf("\n%d FAILED\n", fails + 1);
        return 1;
    }

    // A controller manager of our own: its vtable, slot 0 at +0x10, slot 0 in use (+0xAB28).
    auto* mgr = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0xAC00, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    *reinterpret_cast<uintptr_t*>(mgr) = reinterpret_cast<uintptr_t>(game) + 0x2E7A2E8;
    unsigned char* slot                = mgr + 0x10;
    *reinterpret_cast<unsigned char**>(mgr + 0xAB28) = slot;
    DWORD old = 0;
    VirtualProtect(game + 0x365D7C8, 8, PAGE_READWRITE, &old);
    *reinterpret_cast<unsigned char**>(game + 0x365D7C8) = mgr;
    // Slot 8 as a second copy of the same pad (the game keeps one pad in more than one slot): it's hidden
    // there too, but its presses mustn't count twice.
    unsigned char* mirror = mgr + 0x10 + 8 * 0x1300;
    auto release = reinterpret_cast<void (*)(void*)>(game + 0x10768D0);
    struct After {
        uint64_t taken, held;
        uint16_t raw;
        uint32_t dpad_dir;   // the reading's d-pad as one control: 8 = idle
    };
    auto frame = [&](uint64_t controls) {
        for (unsigned char* s : {slot, mirror}) {
            *reinterpret_cast<uint64_t*>(s + 0x78) = controls;   // what the game's reading builder left
            *reinterpret_cast<uint16_t*>(s + 0xC0) = static_cast<uint16_t>(controls & 0xF3FF);
            // with the d-pad as one control (30) set, the builder gives it a direction (2) and values
            *reinterpret_cast<uint64_t*>(s + 0x80) = controls & C(30) ? 0x3F80000000000002ull : 8;
            release(s);
        }
        const After a{*reinterpret_cast<uint64_t*>(slot + 0x1E8), *reinterpret_cast<uint64_t*>(slot + 0x78),
                      *reinterpret_cast<uint16_t*>(slot + 0xC0), *reinterpret_cast<uint32_t*>(slot + 0x80)};
        Sleep(60);   // a slow frame, so a press reaches the mod (and its buttons() call) before the next one
        return a;
    };
    // A press reaches the mod on the platform's thread, and the mod's line about it (with what its buttons() call
    // saw) can take longer than a frame to write to a slow disk. Before the frame after a press changes what's
    // held, wait (up to 3 s) until the mod has logged n presses in all.
    auto presses_logged = [](std::size_t n) {
        for (int i = 0; i < 60; ++i) {
            const std::string l = slurp(L"fh2-mods\\platform.log");
            std::size_t c       = 0;
            for (std::size_t at = 0; (at = l.find("[PadMod] press ", at)) != std::string::npos; ++at) ++c;
            if (c >= n) return;
            Sleep(50);
        }
    };
    auto is = [](const After& a, uint64_t taken, uint64_t held) {
        return a.taken == taken && a.held == held && a.raw == static_cast<uint16_t>(held & 0xF3FF);
    };

    const int kUp = 0, kLeft = 2, kRight = 3, kLB = 8, kA = 12;
    (void)kUp; (void)kRight;
    CHECK(is(frame(C(kLeft)), 0, C(kLeft)));                         // Left alone: the game has it
    CHECK(is(frame(0), 0, 0));
    CHECK(is(frame(C(kLB)), 0, C(kLB)));                             // LB alone: nothing hidden
    CHECK(is(frame(C(kLB) | C(kLeft)), C(kLeft), C(kLB)));           // LB held, Left pressed: hidden and taken
    CHECK(is(frame(C(kLB) | C(kLeft)), C(kLeft), C(kLB)));           // still held: still hidden, no second press
    CHECK(is(frame(C(kLeft)), C(kLeft), 0));                         // LB let go first: Left stays hidden...
    CHECK(is(frame(0), 0, 0));                                       // ...until it goes up (the game clears taken)
    CHECK(is(frame(C(kLeft)), 0, C(kLeft)));                         // pressed again without LB: the game's
    CHECK(is(frame(C(kLB) | C(kA)), 0, C(kLB) | C(kA)));             // A isn't the layer's
    CHECK(is(frame(C(kLB) | C(34)), C(34), C(kLB)));                 // Up+Right, the diagonal control 34: hidden
    presses_logged(3);                                               // Up's and Right's buttons() while held
    CHECK(is(frame(C(kLB)), 0, C(kLB)));                             // and cleared by the game when let go
    *reinterpret_cast<uint64_t*>(slot + 0x1E8) = C(kA);             // something the game itself took...
    CHECK(is(frame(C(kLB) | C(kA)), C(kA), C(kLB) | C(kA)));         // ...stays taken while held, untouched
    CHECK(is(frame(C(kLB)), 0, C(kLB)));                             // ...and goes when let go
    // Drive is for the driving view only, and the pop-ups follow the HUD page here: none has been shown, so RB + A
    // is the game's and no press comes. Where the page couldn't be followed, the layer is open as any other.
    const int kRB = 9;
    const bool followed = has(log, "the HUD page tracked");
    std::printf("the HUD page is %s in this run\n", followed ? "followed (not on screen)" : "not followed");
    if (followed) CHECK(is(frame(C(kRB) | C(kA)), 0, C(kRB) | C(kA)));    // not the driving view: A is the game's
    else CHECK(is(frame(C(kRB) | C(kA)), C(kA), C(kRB)));                 // nothing to go by: open everywhere
    CHECK(is(frame(0), 0, 0));
    // The latched layers, which PadMod keeps open: Editor takes X with no button held, wherever the player is; Flight
    // is for the driving view only, so with the HUD page followed and not on screen, Y is the game's.
    const int kX = 14, kY = 15;
    for (int i = 0; i < 60 && !has(slurp(L"fh2-mods\\platform.log"), "[PadMod] set_open: Editor and Flight are open"); ++i) Sleep(50);
    CHECK(has(slurp(L"fh2-mods\\platform.log"), "[PadMod] pad layer Editor opened"));
    CHECK(is(frame(C(kX)), C(kX), 0));                                    // X alone: hidden, the layer's
    presses_logged(4);
    CHECK(has(slurp(L"fh2-mods\\platform.log"), "[PadMod] press Editor 0x4000"));
    CHECK(is(frame(0), 0, 0));
    if (followed) CHECK(is(frame(C(kY)), 0, C(kY)));                      // not the driving view: Y is the game's
    else CHECK(is(frame(C(kY)), C(kY), 0));
    CHECK(is(frame(0), 0, 0));
    // the d-pad as one control (30, its left-right axis 31): the radio reads that
    After a = frame(C(kRight) | C(30) | C(31));
    CHECK(is(a, 0, C(kRight) | C(30) | C(31)) && a.dpad_dir == 2);   // without LB: the game's
    CHECK(is(frame(0), 0, 0));
    a = frame(C(kLB) | C(kRight) | C(30) | C(31));
    CHECK(is(a, C(kRight), C(kLB)) && a.dpad_dir == 8);              // LB + Right: all of it hidden, the d-pad idle
    presses_logged(5);
    a = frame(C(kLB));
    CHECK(is(a, 0, C(kLB)) && a.dpad_dir == 8);

    // The game's memory not as the platform expects. It reads the manager, the slot in use and the reading through
    // safe_copy, and writes the slot inside a guard, so neither faults: nothing is hidden and no press counts.
    // The manager pointer at memory that can't be read (freed, say):
    auto* gone = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    *reinterpret_cast<unsigned char**>(game + 0x365D7C8) = gone;
    CHECK(is(frame(C(kLB) | C(kLeft)), 0, C(kLB) | C(kLeft)));       // the game keeps Left
    CHECK(is(frame(C(kLB)), 0, C(kLB)));
    *reinterpret_cast<unsigned char**>(game + 0x365D7C8) = mgr;
    // The slot read-only (the game's own function writes nothing to it with nothing taken):
    *reinterpret_cast<uint64_t*>(slot + 0x78) = C(kLB) | C(kLeft);
    *reinterpret_cast<uint16_t*>(slot + 0xC0) = static_cast<uint16_t>((C(kLB) | C(kLeft)) & 0xF3FF);
    VirtualProtect(mgr, 0xAC00, PAGE_READONLY, &old);
    release(slot);
    VirtualProtect(mgr, 0xAC00, PAGE_READWRITE, &old);
    CHECK(*reinterpret_cast<uint64_t*>(slot + 0x1E8) == 0 && *reinterpret_cast<uint64_t*>(slot + 0x78) == (C(kLB) | C(kLeft)));
    CHECK(is(frame(C(kLB)), 0, C(kLB)));                             // and on as before once it's back

    Sleep(500);
    log = slurp(L"fh2-mods\\platform.log");
    std::printf("---- platform.log\n%s----\n", log.c_str());
    // one press each: Left, then Up and Right from the diagonal, then the latched layer's X; buttons() saw what was held at the time
    CHECK(has(log, "[PadMod] press Chat 0x0004 (buttons 1 0x010"));
    CHECK(has(log, "[PadMod] press Chat 0x0001 (buttons 1 0x0109)"));
    CHECK(has(log, "[PadMod] press Chat 0x0008 (buttons 1 0x0109)"));
    CHECK(has(log, "[PadMod] press Chat 0x0008 (buttons 1 0x0108)"));   // the Right with the d-pad as one
    std::size_t presses = 0;
    for (std::size_t at = 0; (at = log.find("[PadMod] press ", at)) != std::string::npos; ++at) ++presses;
    CHECK(has(log, "[PadMod] press Editor 0x4000 (buttons 1 0x4000)"));   // the latched layer's X, with nothing else held
    CHECK(presses == 5);
    CHECK(!has(log, "crashed in its on_press"));
    // its pop-up, sent mid-session without declaring pop-ups (it declared tips, so the hooks are in): queued, with a
    // line asking it to declare them, and ui_status says they can show
    CHECK(has(log, "[PadMod] shows pop-ups without saying so"));
    CHECK(has(log, "[PadMod] pop-up queued: Pad layer / A press (key Press)"));
    CHECK(has(log, "[PadMod] show_toast 1, then ui_status has pop-ups: yes"));
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails;
}
