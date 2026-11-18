// play_ui_sound (API 10, platform 2.9.0): the game's own UI sounds, for every mod, with no HUD layer needed.
//
// The sounds go the way the MODS tab's own and the HUD layer preview's go (settings.cpp's play_sound_inner): an MSVC
// std::string of the sound's FMOD name, made with the game's assign (game+0x22C300), handed by value to the
// AudioManagerDeferred at game+0x365A298, its vtable +0x08 (game+0xF71C50), which takes and frees it. Static
// research S4 found every one of the eleven played by the game itself through that object and method, and the HUD
// layer preview has played and heard all eleven in game that way (docs/hud-layer-preview.md: ERROR, INFO and SHUTTER
// under the pause menu too, NOTIFY and the countdown beeps only over the driving view).
//
// That player's queue has no lock (S4 2.1), so sounds are played only on the game's thread:
//  - from a mod's page, setting, title-screen or message box callback (ui_callback_now: the game's UI thread), at
//    once;
//  - from anywhere else, at the game's next tick: an instruction hook on the AchievementsModule update
//    (game+0x3FCD84, the pop-ups' and the HUD layer's tick, chained with them and with Achievements), made the first
//    time a mod plays a sound from outside a callback, so a session where none does leaves the site alone.
// The rules (repeats, the world-only four, the half second) are ui_rules.cpp's UiSoundRules. Every address is
// checked against the exe on disk before the first sound plays, and the player's vtable and each name slot again
// in memory as it plays; a mismatch, or a crash in the game's code, turns play_ui_sound off with a log line.
#include "ui_sounds.hpp"

#include "platform.hpp"
#include "popups.hpp"
#include "ui_rules.hpp"
#include "ui_templates.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fh2platform {
namespace {

constexpr uintptr_t kImageBase          = 0x140000000;
constexpr uintptr_t RVA_SOUND_PLAYER    = 0x365A298;   // 0x14365A298 the AudioManagerDeferred
constexpr uintptr_t RVA_SOUND_PLAYER_VT = 0x2E6AE88;   // 0x142E6AE88 .?AVAudioManagerDeferred@@
constexpr uintptr_t RVA_SOUND_PLAY      = 0xF71C50;    // 0x140F71C50 its vtable +0x08
constexpr uintptr_t RVA_STRING_ASSIGN   = 0x22C300;    // 0x14022C300 std::string::assign
constexpr uintptr_t RVA_TICK_FN         = 0x3FCD60;    // 0x1403FCD60 AchievementsModule +0x20
constexpr uintptr_t RVA_TICK            = 0x3FCD84;    // 0x1403FCD84 its mov eax,[rcx+0xA8]

const char* const kWho = "UI sounds";
FH2Mod make_owner() {
    FH2Mod m;
    m.name          = kWho;
    m.wname         = L"UI sounds";
    m.check_updates = false;
    return m;
}
FH2Mod g_owner = make_owner();   // owns the tick's hook; never registered
std::atomic<const FH2ModApi*> g_api{nullptr};
std::atomic<uintptr_t> g_base{0};
UiSoundRules g_rules;

// 0 not checked yet, 1 the game's sound code is as expected, -1 it isn't (or a sound crashed in it)
std::atomic<int> g_code{0};
std::mutex g_check_mu;
// the tick: 0 not asked for, 1 on its way, 2 in, -1 it couldn't be hooked
std::atomic<int> g_tick{0};
// settings.cpp's page texts (API 11) are refreshed on the tick too: called at every tick once asked for (ui_tick_also)
std::atomic<void (*)()> g_tick_also{nullptr};

double now_ms() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

uintptr_t rdp(uintptr_t a) {
    uintptr_t v = 0;
    return a >= 0x10000 && safe_copy(&v, reinterpret_cast<const void*>(a), sizeof v) ? v : 0;
}

// Each mod's lines that are said once (a reason, or a sound's first play).
std::mutex g_said_mu;
std::set<std::pair<const FH2Mod*, std::string>> g_said;
void say_once(FH2Mod* m, const std::string& what, const std::string& line) {
    {
        std::scoped_lock lk{g_said_mu};
        if (!g_said.insert({m, what}).second) return;
    }
    log_line(m ? m->name : std::string(kWho), line);
}

// ---- the code check (against the exe on disk) -------------------------------------------------------------------

std::size_t parse_hex(const char* s, uint8_t* out, std::size_t cap) {
    std::size_t n = 0;
    while (*s && n < cap) {
        while (*s == ' ') ++s;
        if (!s[0] || !s[1]) break;
        const char b[3] = {s[0], s[1], 0};
        out[n++]        = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
        s += 2;
    }
    return n;
}

bool disk_has(uintptr_t base, uintptr_t rva, const void* want, std::size_t n, const char* what) {
    if (disk_bytes_equal(reinterpret_cast<const void*>(base + rva), want, n)) return true;
    char b[160];
    std::snprintf(b, sizeof b, "code: game+0x%llX (%s) doesn't match the exe on disk; play_ui_sound plays nothing this session",
                  static_cast<unsigned long long>(rva), what);
    log_line(kWho, b);
    return false;
}
bool disk_bytes(uintptr_t base, uintptr_t rva, const char* hex, const char* what) {
    uint8_t want[64];
    const std::size_t n = parse_hex(hex, want, sizeof want);
    return disk_has(base, rva, want, n, what);
}
bool disk_qword(uintptr_t base, uintptr_t rva, uint64_t value, const char* what) { return disk_has(base, rva, &value, 8, what); }

// Whether the game's sound code is as expected: checked once, the first time it's needed, once the game is there.
bool code_ok() {
    const int c = g_code.load();
    if (c) return c > 0;
    const FH2ModApi* api = g_api.load();
    void* base           = api ? api->wait_for_code(0) : nullptr;
    if (!base) return false;   // not yet: asked again next time
    std::scoped_lock lk{g_check_mu};
    if (g_code.load()) return g_code.load() > 0;
    g_base = reinterpret_cast<uintptr_t>(base);
    const uintptr_t b = g_base.load();
    if (!disk_image_readable()) {
        log_line(kWho, "the game exe couldn't be read from the game folder, so the game's sound code can't be checked; play_ui_sound plays nothing this session");
        g_code = -1;
        return false;
    }
    bool ok = disk_qword(b, RVA_SOUND_PLAYER_VT + 8, kImageBase + RVA_SOUND_PLAY, "the sound player's play") &&
              disk_bytes(b, RVA_SOUND_PLAY, "48 89 54 24 10 55 56 57 48 83 EC 60", "the sound player's play") &&
              disk_bytes(b, RVA_STRING_ASSIGN, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8", "std::string::assign");
    for (uint32_t s = FH2_SOUND_SELECT; ok && s <= FH2_SOUND_SHUTTER; ++s) {
        const UiSoundInfo& i = kUiSounds[s];
        if (!i.slot) continue;
        ok = disk_qword(b, i.slot, kImageBase + i.text_rva, i.name) && disk_has(b, i.text_rva, i.text, std::strlen(i.text) + 1, i.name);
    }
    g_code = ok ? 1 : -1;
    if (ok) log_line(kWho, "code: the game's sound player and the eleven sounds' names are as expected");
    return ok;
}

// ---- playing -----------------------------------------------------------------------------------------------------

enum class Played { yes, not_up, shape, crashed, code };

// On the game's thread only. The player's vtable and the sound's name slot are read again in memory first.
Played play(uint32_t sound, uint32_t* code) {
    *code = 0;
    if (!code_ok()) return Played::code;
    const uintptr_t b = g_base.load();
    const uintptr_t player = rdp(b + RVA_SOUND_PLAYER);
    if (!player) return Played::not_up;
    if (rdp(player) != b + RVA_SOUND_PLAYER_VT) return Played::shape;
    const UiSoundInfo& i = kUiSounds[sound];
    if (i.slot) {
        char got[96]        = {};
        const std::size_t n = std::strlen(i.text) + 1;
        if (rdp(b + i.slot) != b + i.text_rva || !safe_copy(got, reinterpret_cast<const void*>(b + i.text_rva), n) || std::memcmp(got, i.text, n) != 0)
            return Played::shape;
    }
    if (settings_play_sound_unchecked(b, i.text, code)) return Played::yes;
    if (!*code) return Played::not_up;
    g_code = -1;
    char line[160];
    std::snprintf(line, sizeof line, "sound %s crashed in the game's code (exception 0x%08X); play_ui_sound plays nothing more this session", i.name, *code);
    log_line(kWho, line);
    return Played::crashed;
}

// What a played sound or a refusal says in the mod's log, once each.
void tell(FH2Mod* m, uint32_t sound, Played p) {
    const std::string name = kUiSounds[sound].name;
    switch (p) {
        case Played::yes: say_once(m, "played " + name, "UI sound " + name + " played (said once for each sound)"); break;
        case Played::not_up: say_once(m, "not up", "play_ui_sound: the game's sound isn't up yet, so " + name + " wasn't played"); break;
        case Played::shape:
            say_once(m, "shape " + name, "play_ui_sound: the game's sound player or the name of " + name + " isn't as expected in memory, so it wasn't played");
            break;
        case Played::crashed:
        case Played::code:
            say_once(m, "code", "play_ui_sound: the game's sound code isn't as expected this session (see the UI sounds lines), so no sound plays");
            break;
    }
}

// ---- the tick ----------------------------------------------------------------------------------------------------

void on_tick(FH2Context*, void*) {
    if (auto also = g_tick_also.load()) also();
    if (!g_rules.waiting()) return;
    std::vector<UiSoundRules::Dropped> dropped;
    UiSoundRules::Due due;
    const bool any = g_rules.tick(now_ms(), popups_hud_on_screen(), due, dropped);
    for (const UiSoundRules::Dropped& d : dropped) {
        FH2Mod* m              = const_cast<FH2Mod*>(static_cast<const FH2Mod*>(d.mod));
        const std::string name = kUiSounds[d.sound].name;
        if (d.why == UiSoundRules::Drop::late)
            say_once(m, "late " + name, "UI sound " + name + " dropped: the game's tick didn't come within 0.5 s (said once)");
        else
            say_once(m, "gone " + name, "UI sound " + name + " dropped: the driving view went off screen before it could play (said once)");
    }
    if (!any) return;
    FH2Mod* m = const_cast<FH2Mod*>(static_cast<const FH2Mod*>(due.mod));
    if (stopped_online(m)) return;   // stopped for online play just after it asked (go_online drops what waits)
    uint32_t code = 0;
    tell(m, due.sound, play(due.sound, &code));
}

// The tick's hook, the first time a sound waits for it (on a thread of its own, so play_ui_sound stays quick).
void want_tick() {
    int none = 0;
    if (!g_tick.compare_exchange_strong(none, 1)) return;
    std::thread([] {
        const FH2ModApi* api = g_api.load();
        const uintptr_t b    = g_base.load();
        if (!api || !b || !disk_bytes(b, RVA_TICK_FN, "40 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF", "the AchievementsModule update") ||
            !disk_bytes(b, RVA_TICK, "8B 81 A8 00 00 00", "the AchievementsModule update")) {
            g_tick = -1;
            log_line(kWho, "the game's tick isn't as expected, so sounds from outside a mod's callbacks don't play this session");
            return;
        }
        FH2Hook* h = api->hook_instruction(&g_owner, "AchievementsModule update (play_ui_sound's tick)", reinterpret_cast<void*>(b + RVA_TICK), &on_tick, nullptr);
        if (!h || !api->enable_hooks(&g_owner, h)) {
            if (h) api->disable_hooks(&g_owner, h);
            g_tick = -1;
            log_line(kWho, "the game's tick couldn't be hooked (see the line above), so sounds from outside a mod's callbacks don't play this session");
            return;
        }
        g_tick = 2;
        log_line(kWho, "the tick is in: sounds from outside a mod's callbacks play at the game's next tick");
    }).detach();
}

} // namespace

int ui_sounds_play(FH2Mod* m, uint32_t sound) {
    if (!m) return 0;
    if (stopped_online(m)) {
        say_once(m, "online", "play_ui_sound: this mod was stopped for online play, so its sounds don't play");
        return 0;
    }
    if (!ui_sound_known(sound)) {
        say_once(m, "unknown", "play_ui_sound: " + std::to_string(sound) + " isn't a sound this platform knows (FH2_SOUND_SELECT to FH2_SOUND_SHUTTER)");
        return 0;
    }
    const std::string name = kUiSounds[sound].name;
    if (!g_api.load() || !g_api.load()->wait_for_code(0)) {
        say_once(m, "no game", "play_ui_sound: the game isn't running yet, so " + name + " wasn't played");
        return 0;
    }
    if (!code_ok()) {
        tell(m, sound, Played::code);
        return 0;
    }
    if (!rdp(g_base.load() + RVA_SOUND_PLAYER)) {
        tell(m, sound, Played::not_up);
        return 0;
    }
    const UiCallbackState* cb = ui_callback_now();
    const bool in_callback    = cb != nullptr;
    const int on_screen       = popups_hud_on_screen();
    switch (g_rules.ask(m, sound, now_ms(), in_callback, on_screen)) {
        case UiSoundRules::Ask::unknown: return 0;
        case UiSoundRules::Ask::repeat:
            say_once(m, "repeat", "play_ui_sound: the same sound again within 100 ms isn't played (said once)");
            return 0;
        case UiSoundRules::Ask::world_only:
            say_once(m, "world " + name, "play_ui_sound: " + name + " sounds only over the driving view, and " +
                                             (in_callback ? "a menu is up (this is one of the mod's callbacks)" : "the driving view isn't on screen") +
                                             ", so it wasn't played (said once)");
            return 0;
        case UiSoundRules::Ask::play_now: {
            uint32_t code   = 0;
            const Played p = play(sound, &code);
            tell(m, sound, p);
            return p == Played::yes ? 1 : 0;
        }
        case UiSoundRules::Ask::queued:
            if (g_tick.load() < 0) {
                say_once(m, "no tick", "play_ui_sound: the game's tick couldn't be hooked this session, so sounds from outside the mod's callbacks don't play");
                return 0;
            }
            want_tick();
            return 1;
    }
    return 0;
}

bool ui_sound_play_direct(uint32_t sound, uint32_t* code) {
    *code = 0;
    return ui_sound_known(sound) && play(sound, code) == Played::yes;
}

void ui_sounds_prepare(const FH2ModApi* api) { g_api = api; }

bool ui_tick_also(void (*fn)()) {
    g_tick_also = fn;
    if (g_tick.load() == 0) {
        const FH2ModApi* api = g_api.load();
        void* base           = api ? api->wait_for_code(0) : nullptr;
        if (!base) return true;   // the game isn't there yet: asked again with the next change
        g_base = reinterpret_cast<uintptr_t>(base);
        want_tick();
    }
    return g_tick.load() >= 0;
}

void ui_sounds_mod_stopped(FH2Mod* m) {
    if (const std::size_t n = m ? g_rules.forget(m) : 0)
        log_line(m->name, "stopped for online play, so its waiting UI sounds were dropped (" + std::to_string(n) + ")");
}

} // namespace fh2platform
