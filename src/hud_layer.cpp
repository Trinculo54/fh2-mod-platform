// The HUD layer preview (include/fh2mod_hud.h): a heads-up display of a mod's own, drawn by the game's
// own UI inside its free-roam HUD page (A_Hud.xaml), offered through the FH2Platform.HudLayer interface
// (v1), not the API. This file is the glue between hud_state.cpp (what the layer shows, and when the
// page is told) and the game:
//  - add_hud_layer, from fh2mod_init only; once every mod's fh2mod_init has returned, the page gets one
//    copy of the layer (hud_template.cpp) with the union of the layers' parts, checked before set_file.
//    A crash marker leaves the layer out for one session after any crash between start-up and the HUD
//    page's first 10 s on screen, whatever the crash was.
//  - FindProperty (hooked by the platform for the Mods page) answers the FH2Mod_Hud_ names with
//    descriptors of the layer's own. Their getter gives the free-roam HUD page (Hud, vtable
//    0x142DEF9C8) the published values and every other page the defaults; Strings are the game's own.
//  - The pump: an instruction hook on the AchievementsModule's tick (0x1403FCD84, inside CForza::Update's
//    UI lock, once a frame), shared with Achievements. It runs the state, tells the live page what
//    changed, holds the game's own HUD hidden while a mod asks, and plays the game's UI sounds.
//  - The page is tracked through its vtable's shown (+0x488), hidden (+0x4F8) and deleting destructor
//    (slot 0).
// Without a mod that adds a layer, nothing here runs: no hook, no thread, no file. Every game address
// is checked against the exe on disk before it's used, and a mismatch turns off only what needs it.
// Game memory is read through safe_copy, and every game call goes through a small SEH-guarded helper
// that holds no object with a destructor.
#include "hud_layer.hpp"

#include "fh2mod_hud.h"
#include "hud_state.hpp"
#include "hud_template.hpp"
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh2platform {
namespace {

namespace fs = std::filesystem;

// The header's layout is frozen: a mod built against it must find every member where it put it.
static_assert(sizeof(FH2HudPrompt) == 40, "FH2HudPrompt moved");
static_assert(sizeof(FH2HudMark) == 12, "FH2HudMark moved");
static_assert(sizeof(FH2HudChip) == 16, "FH2HudChip moved");
static_assert(sizeof(FH2HudRow) == 32, "FH2HudRow moved");
static_assert(sizeof(FH2HudLayer) == 32, "FH2HudLayer moved");
static_assert(sizeof(FH2HudState) == 1000, "FH2HudState moved");
static_assert(offsetof(FH2HudState, countdown_label) == 992, "FH2HudState moved");
static_assert(sizeof(FH2HudFeedLine) == 32, "FH2HudFeedLine moved");
static_assert(offsetof(FH2HudInterface, play_ui_sound) == 64, "FH2HudInterface moved");   // version 1 ends here
static_assert(sizeof(FH2HudInterface) == 80, "FH2HudInterface moved");

// ---- game addresses (game+RVA; the full address in each comment) ------------------------------------
constexpr uintptr_t kImageBase = 0x140000000;
// the pump
constexpr uintptr_t RVA_TICK_FN = 0x3FCD60;           // 0x1403FCD60 AchievementsModule +0x20
constexpr uintptr_t RVA_TICK    = 0x3FCD84;           // 0x1403FCD84 its mov eax,[rcx+0xA8], reached every frame in the UI lock
// the free-roam HUD page (Hud, vtable 0x142DEF9C8)
constexpr uintptr_t RVA_HUD_VT          = 0x2DEF9C8;  // 0x142DEF9C8 vtable; slot 0 = deleting destructor
constexpr uintptr_t RVA_HUD_DTOR        = 0xD244C0;   // 0x140D244C0
constexpr uintptr_t RVA_HUD_SHOWN_SLOT  = 0x2DEFE50;  // 0x142DEFE50 vtable +0x488
constexpr uintptr_t RVA_HUD_SHOWN       = 0xD38DD0;   // 0x140D38DD0 (writes page+0x410 = 1)
constexpr uintptr_t RVA_HUD_HIDDEN_SLOT = 0x2DEFEC0;  // 0x142DEFEC0 vtable +0x4F8
constexpr uintptr_t RVA_HUD_HIDDEN      = 0xD41540;   // 0x140D41540 (writes page+0x410 = 0)
constexpr std::size_t kHudShownByte     = 0x410;
constexpr uintptr_t RVA_LIMITED_HUD_VT   = 0x2DF3F28; // 0x142DF3F28 LimitedHud (recognised in logs only)
constexpr uintptr_t RVA_LIMITED_MINOR_VT = 0x2DF4798; // 0x142DF4798 LimitedHudMinorNotification
constexpr uintptr_t RVA_TELEMETRY_VT     = 0x2E555E8; // 0x142E555E8 Telemetry
// boxing, checked by the platform before it hooked FindProperty (settings.cpp hook_game)
constexpr uintptr_t RVA_BOX_INT   = 0x4702D0;         // 0x1404702D0 box(out, const int*)
constexpr uintptr_t RVA_BOX_BOOL  = 0x336BA0;         // 0x140336BA0 box(out, const bool*)
constexpr uintptr_t RVA_BOX_FLOAT = 0x3F5990;         // 0x1403F5990 box(out, const float*)
// AVUI Strings
constexpr uintptr_t RVA_AVUI_ALLOC   = 0x14424E0;     // 0x1414424E0 alloc(size, type)
constexpr uintptr_t RVA_STRING_TYPE  = 0x16A4B30;     // 0x1416A4B30 the String type
constexpr uintptr_t RVA_STRING_WCTOR = 0x1663340;     // 0x141663340 String(obj, const wchar_t*, length)
constexpr uintptr_t RVA_OBJECT_FREE  = 0x257730;      // 0x140257730 an AVUI object at refcount 0
// the UI lock
constexpr uintptr_t RVA_UI_GLOBAL = 0x36B0418;        // 0x1436B0418: [[it]+0x30]+0x160 is the UI lock, owner token at +0
// the game's HUD modules and the hold
constexpr uintptr_t RVA_WM_PTR          = 0x34C0F40;  // 0x1434C0F40 the WorldManager pointer
constexpr uintptr_t RVA_WM_VT           = 0x2CD1F20;  // 0x142CD1F20 .?AVWorldManager@EntityModel@@
constexpr uintptr_t RVA_HUDMODULE_VT    = 0x2CDC7E0;  // 0x142CDC7E0 .?AVHUDModule@EntityModel@@
constexpr uintptr_t RVA_HUDMODULE_AR_VT = 0x2CDC890;  // 0x142CDC890 .?AVHUDModuleAR@EntityModel@@
constexpr uintptr_t RVA_PRIO_VT         = 0x2CDC5E8;  // 0x142CDC5E8 .?AVHUDStatusPrioritiserModule@EntityModel@@
constexpr uintptr_t RVA_SUSPEND         = 0x78B0F0;   // 0x14078B0F0 suspend(HUDModule*): prioritiser+0xA8 = 1
constexpr uintptr_t RVA_SUSPEND_AR      = 0x78B1A0;   // 0x14078B1A0 suspend(HUDModuleAR*)
constexpr uintptr_t RVA_RESUME          = 0x785CD0;   // 0x140785CD0 resume(either module)
constexpr std::size_t kPrioSuspended    = 0xA8;
// UI sounds
constexpr uintptr_t RVA_SOUND_PLAYER    = 0x365A298;  // 0x14365A298 the AudioManagerDeferred the platform plays through
constexpr uintptr_t RVA_SOUND_PLAYER_VT = 0x2E6AE88;  // 0x142E6AE88 .?AVAudioManagerDeferred@@
constexpr uintptr_t RVA_SOUND_PLAY      = 0xF71C50;   // 0x140F71C50 its vtable +8
constexpr uint32_t kDescMagic           = 0x31484C46; // "FLH1" at descriptor +0x7F8, the property's index at +0x800

// ---- small helpers ----------------------------------------------------------------------------------

FH2Mod make_owner() {
    FH2Mod m;
    m.name          = "HUD layer";
    m.wname         = L"HUD layer";
    m.check_updates = false;
    return m;
}
FH2Mod g_layer_mod = make_owner();   // owns the layer's hooks and its title notice; never registered
const FH2ModApi* g_api = nullptr;
std::atomic<uintptr_t> g_base{0};
uintptr_t G(uintptr_t rva) { return g_base.load(std::memory_order_relaxed) + rva; }
void* GP(uintptr_t rva) { return reinterpret_cast<void*>(G(rva)); }

LARGE_INTEGER qpf() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
}
LARGE_INTEGER qpc() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c;
}
const LARGE_INTEGER g_qpf = qpf(), g_q0 = qpc();
double now_ms() {
    const LARGE_INTEGER q = qpc();
    return static_cast<double>(q.QuadPart - g_q0.QuadPart) * 1000.0 / static_cast<double>(g_qpf.QuadPart);
}

__attribute__((format(printf, 1, 2))) void hlog(const char* fmt, ...) {
    char b[2048];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    log_line(g_layer_mod.name, b);
}

unsigned long long ull(uintptr_t v) { return static_cast<unsigned long long>(v); }
unsigned long long ullp(const void* p) { return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)); }
unsigned long long u64(uint64_t v) { return static_cast<unsigned long long>(v); }

template <class T> bool rd(uintptr_t a, T& v) { return a >= 0x10000 && safe_copy(&v, reinterpret_cast<const void*>(a), sizeof v); }
template <class T> bool rd(const void* p, T& v) { return rd(reinterpret_cast<uintptr_t>(p), v); }
uintptr_t rdp(uintptr_t a) {
    uintptr_t v = 0;
    return rd(a, v) ? v : 0;
}
uintptr_t vtable_of(const void* obj) { return obj ? rdp(reinterpret_cast<uintptr_t>(obj)) : 0; }

fs::path fh2mods() { return azimuth_dir() / L"fh2-mods"; }
bool file_exists(const fs::path& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Guarded game calls: one SEH frame for all of them, each call a captureless lambda over a plain context.
bool guarded(void (*thunk)(void*), void* ctx, DWORD* code) {
    __try {
        thunk(ctx);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct Call1 {
    uintptr_t fn;
    void* a;
};
// fn(a) in the game; false (with the exception) if it faulted.
bool call1(uintptr_t rva, void* a, DWORD* code) {
    Call1 c{G(rva), a};
    return guarded(
        [](void* p) {
            auto* c = static_cast<Call1*>(p);
            reinterpret_cast<void (*)(void*)>(c->fn)(c->a);
        },
        &c, code);
}

// ---- code checks against the exe on disk ------------------------------------------------------------

struct Entry {
    char kind;          // 'B' bytes, 'Q' a qword (a vtable slot), 'T' an E8 rel32 to a target, 'S' a C string
    uintptr_t rva;
    const char* hex;    // B: the bytes; S: the text (its NUL is compared too)
    uint64_t value;     // Q: the qword; T: the full target address
};
Entry B(uintptr_t rva, const char* hex) { return {'B', rva, hex, 0}; }
Entry Q(uintptr_t rva, uint64_t v) { return {'Q', rva, nullptr, v}; }
Entry T(uintptr_t rva, uint64_t target) { return {'T', rva, nullptr, target}; }
Entry S(uintptr_t rva, const char* text) { return {'S', rva, text, 0}; }

std::size_t parse_hex(const char* s, uint8_t* out, std::size_t cap) {
    std::size_t n = 0;
    while (*s && n < cap) {
        while (*s == ' ') ++s;
        if (!s[0] || !s[1]) break;
        char b[3] = {s[0], s[1], 0};
        out[n++]  = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
        s += 2;
    }
    return n;
}

// True if every entry matches the exe on disk; otherwise false and the first mismatch's address.
bool entries_match(const std::vector<Entry>& es, uintptr_t* bad) {
    for (const Entry& e : es) {
        uint8_t want[256];
        std::size_t n = 0;
        if (e.kind == 'B') {
            n = parse_hex(e.hex, want, sizeof want);
        } else if (e.kind == 'S') {
            n = std::min(std::strlen(e.hex) + 1, sizeof want);
            std::memcpy(want, e.hex, n);
        } else if (e.kind == 'Q') {
            std::memcpy(want, &e.value, 8);
            n = 8;
        } else {
            const int64_t rel = static_cast<int64_t>(e.value) - static_cast<int64_t>(kImageBase + e.rva + 5);
            const auto r32    = static_cast<int32_t>(rel);
            want[0]           = 0xE8;
            std::memcpy(want + 1, &r32, 4);
            n = 5;
        }
        if (!n || !disk_bytes_equal(GP(e.rva), want, n)) {
            *bad = e.rva;
            return false;
        }
    }
    return true;
}

// One group's code: logged either way (L15).
bool check_group(const char* group, const char* what_goes, const std::vector<Entry>& es) {
    uintptr_t bad = 0;
    if (entries_match(es, &bad)) {
        hlog("code: %s: as expected", group);
        return true;
    }
    hlog("code: %s: game+0x%llX doesn't match the exe on disk; %s", group, ull(bad), what_goes);
    return false;
}

// The seven sounds play_ui_sound plays (FH2_SOUND_ERROR to _SHUTTER): the game's name slots and texts.
struct Sound {
    uintptr_t slot;       // the game's qword pointing at the name
    uintptr_t text_rva;   // where it points
    const char* text;
};
const Sound kSounds[7] = {
    {0x2C5EFB8, 0x1E0A350, "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_Fail_01"},
    {0x2C5F008, 0x1E0A5F8, "/Master/UI/Notification/HZ2_SFX_HUD_Notification_GenericOn_01"},
    {0x2C5F068, 0x1E0A568, "/Master/UI/PopUpInfo/HZ2_SFX_HUD_PopUpInfo_Open_01"},
    {0x2C5F208, 0x1E0B018, "/Master/UI/Countdown/HZ2_SFX_HUD_Countdown_High_01"},
    {0x2C5F210, 0x1E0B050, "/Master/UI/Countdown/HZ2_SFX_HUD_Countdown_Low_01"},
    {0x2C5F218, 0x1E0B088, "/Master/UI/Countdown/HZ2_SFX_HUD_Countdown_End_01"},
    {0x2C5F0C0, 0x1E0A9E8, "/Master/UI/Promo/HZ2_SFX_HUD_Promo_TakePhoto_01"},
};

// ---- the session's state ----------------------------------------------------------------------------

std::atomic<bool> g_active{false};      // a layer was added and prepare ran
bool g_left_out    = false;             // the crash marker left the layer out (prepare)
bool g_spliced     = false;             // this session's A_Hud.xaml has the layer
bool g_page_written = false;
bool g_l13_logged  = false;
uint32_t g_parts   = 0;
HudCheck g_check;
std::atomic<bool> g_code_ok{false};     // G1-G4 matched: the getter may answer
std::atomic<bool> g_ready{false};       // the hooks are on and FindProperty is hooked: the pump runs
std::atomic<int> g_choice{0};           // the Button prompts row
bool g_ini_bad = false;
std::atomic<bool> g_hold_code_ok{false};
std::map<std::wstring, int> g_names;    // short name -> property (made in prepare, read only afterwards)
std::unique_ptr<std::atomic<uint8_t*>[]> g_descs;   // the descriptors, by property
int g_nprops   = 0;
int g_shown_ix = -1;
std::mutex g_find_mu;
bool g_kind_refused[4] = {};            // g_find_mu
std::atomic<int> g_desc_count{0};
std::atomic<bool> g_box_off[3] = {};    // b, i, f: a box that faulted is never tried again

// ---- the HUD page, the interlock and counters -------------------------------------------------------

std::atomic<void*> g_page{nullptr};           // the tracked Hud page
std::atomic<uint64_t> g_page_gen{0};          // bumped for each page tracked or forgotten
std::atomic<bool> g_on_screen{false};
std::atomic<bool> g_seen_shown{false};        // a shown for a Hud page has been seen
std::atomic<bool> g_page_read{false};         // FH2Mod_Hud_Shown's descriptor was handed out: the game built the page with the layer
std::atomic<double> g_first_on_screen{0};
std::atomic<uint64_t> g_frame{0};
std::atomic<double> g_last_pump{0};
struct ThreadSeen {
    std::atomic<DWORD> id{0};
    std::atomic<bool> mixed{false};
    void see(DWORD t) {
        DWORD none = 0;
        if (!id.compare_exchange_strong(none, t) && none != t) mixed = true;
    }
};
ThreadSeen g_th_pump, g_th_shown, g_th_hidden, g_th_dtor, g_th_get_screen, g_th_get_build;
std::atomic<DWORD> g_last_shown_thread{0};
std::atomic<uint64_t> g_get_screen{0}, g_get_build{0}, g_getter_calls{0};
thread_local bool g_in_notify = false;        // inside the layer's own notify
void* g_notify_page = nullptr;                // the pump's: the page notifies go to

const char* page_class(uintptr_t vt, char* buf, std::size_t n) {
    if (vt == G(RVA_HUD_VT)) return "Hud";
    if (vt == G(RVA_LIMITED_HUD_VT)) return "LimitedHud";
    if (vt == G(RVA_LIMITED_MINOR_VT)) return "LimitedHudMinorNotification";
    if (vt == G(RVA_TELEMETRY_VT)) return "Telemetry";
    std::snprintf(buf, n, "vtable game+0x%llX", ull(vt - g_base.load()));
    return buf;
}

std::string thread_text(const ThreadSeen& t) {
    const DWORD id = t.id.load();
    if (!id) return "none";
    return std::to_string(id) + (t.mixed.load() ? " and others" : "");
}

// ok: the pump's thread is known, and the tracked page's last shown and every getter call made while it
// was on screen (outside the layer's own notify) were on the pump's thread.
bool interlock_ok() {
    const DWORD pump = g_th_pump.id.load();
    if (!pump || g_th_pump.mixed.load()) return false;
    const DWORD shown = g_last_shown_thread.load();
    if (shown && shown != pump) return false;
    const DWORD get = g_th_get_screen.id.load();
    if (get && (get != pump || g_th_get_screen.mixed.load())) return false;
    return true;
}

bool page_is_hud(void* page) { return page && vtable_of(page) == G(RVA_HUD_VT); }

// Pages the getters have seen, with their refusals (for the stats).
struct SeenPage {
    void* page;
    uintptr_t vt;
    uint64_t refusals;
};
std::mutex g_seen_mu;
std::vector<SeenPage> g_seen_pages;
std::atomic<void*> g_last_getter_obj{nullptr};
std::atomic<uintptr_t> g_last_getter_vt{0};

// Pages are freed and new ones built at the same address; only the Hud class's destructor is hooked, so a
// known address coming back with another vtable is a new page too, seen afresh.
void getter_seen(void* obj, uintptr_t vt, bool hud) {
    const DWORD tid = GetCurrentThreadId();
    void* page      = g_page.load();
    if (obj && obj == page && g_on_screen.load()) {
        ++g_get_screen;
        g_th_get_screen.see(tid);
    } else {
        ++g_get_build;
        g_th_get_build.see(tid);
    }
    if (obj == g_last_getter_obj.load() && vt == g_last_getter_vt.load()) {
        if (!hud) {
            std::scoped_lock lk{g_seen_mu};
            for (SeenPage& s : g_seen_pages)
                if (s.page == obj && s.vt == vt) ++s.refusals;
        }
        return;
    }
    g_last_getter_obj = obj;
    g_last_getter_vt  = vt;
    bool first        = false;
    {
        std::scoped_lock lk{g_seen_mu};
        const auto it = std::find_if(g_seen_pages.begin(), g_seen_pages.end(), [&](const SeenPage& s) { return s.page == obj; });
        if (it == g_seen_pages.end()) {
            first = true;
            if (g_seen_pages.size() < 64) g_seen_pages.push_back({obj, vt, hud ? 0u : 1u});
        } else if (it->vt != vt) {
            first = true;   // a new page where an old one was
            *it   = {obj, vt, hud ? 0u : 1u};
        } else if (!hud) {
            ++it->refusals;
        }
    }
    if (first) {
        char b[48];
        hlog("first seen by the getters: 0x%llX (%s)%s", ullp(obj), page_class(vt, b, sizeof b), hud ? "" : ": its bindings get defaults");
    }
    // a Hud page shown before the hooks went in: adopted
    if (hud && !g_seen_shown.load() && !page) {
        uint8_t shown = 0;
        if (rd(reinterpret_cast<uintptr_t>(obj) + kHudShownByte, shown) && shown == 1) {
            void* none = nullptr;
            if (g_page.compare_exchange_strong(none, obj)) {
                ++g_page_gen;
                g_on_screen = true;
                if (g_first_on_screen.load() == 0) g_first_on_screen = now_ms();
                hlog("adopted HUD page 0x%llX (its +0x410 reads 1, shown before the hooks)", ullp(obj));
            }
        }
    }
}

// ---- the crash marker -------------------------------------------------------------------------------

fs::path marker_path() { return fh2mods() / L"platform-hudlayer-marker.txt"; }
int g_marker_n = 0;                             // loading_sessions carried into this session's marker
std::atomic<int> g_marker_state{0};             // 0 none, 1 written, 2 loading, 3 cleared
std::atomic<bool> g_marker_cleared{false};

bool file_time(const fs::path& p, FILETIME* t) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return false;
    *t = a.ftLastWriteTime;
    return true;
}

// The marker as last session left it: true if the layer is left out (L4a, L4b), else false (L3, L4c).
bool check_marker() {
    FILETIME marked{};
    if (!file_time(marker_path(), &marked)) {
        hlog("crash marker: none from last session");
        return false;
    }
    bool loading = false;
    int n        = 0;
    if (FILE* f = _wfopen(marker_path().c_str(), L"rb")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "state loading", 13) == 0) loading = true;
            if (std::strncmp(line, "loading_sessions ", 17) == 0) n = std::atoi(line + 17);
        }
        std::fclose(f);
    }
    std::wstring newest;
    FILETIME newest_t{};
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((fh2mods() / L"crashes" / L"crash-*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (CompareFileTime(&fd.ftLastWriteTime, &newest_t) > 0) {
                newest_t = fd.ftLastWriteTime;
                newest   = fd.cFileName;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const bool crashed = !newest.empty() && CompareFileTime(&newest_t, &marked) > 0;
    if (crashed || (loading && n >= 2)) {
        // the marker's state says how far the page had got: any crash in that window leaves the layer out
        if (crashed && loading)
            hlog("crash marker: the last session crashed while the HUD page with the layer was loading or had only just been shown "
                 "(crash report %s); the layer is left out this session",
                 narrow(newest).c_str());
        else if (crashed)
            hlog("crash marker: the last session crashed after the HUD page with the layer was written, before the game had started "
                 "using it (crash report %s); the crash may have had another cause, but the layer is left out this session to be safe",
                 narrow(newest).c_str());
        else
            hlog("crash marker: the HUD page started loading with the layer in two sessions running and was never on screen for 10 s; "
                 "the layer is left out this session");
        DeleteFileW(marker_path().c_str());
        return true;
    }
    g_marker_n = loading ? n : 0;
    hlog("crash marker: a marker from last session, with no crash report since; the layer stays in");
    return false;
}

// CreateFileW and WriteFile only: the "loading" write happens on the game's UI thread as the page is built.
bool write_marker(bool loading) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char b[320];
    const int n = std::snprintf(b, sizeof b, "written %04u-%02u-%02u %02u:%02u:%02u UTC\r\nsession %s\r\nstate %s\r\nloading_sessions %d\r\n",
                                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, session_id(), loading ? "loading" : "written",
                                loading ? g_marker_n + 1 : g_marker_n);
    HANDLE f = CreateFileW(marker_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        hlog("crash marker: couldn't be written (the layer stays in, with no protection against a crash loop)");
        return false;
    }
    DWORD w       = 0;
    const bool ok = n > 0 && WriteFile(f, b, static_cast<DWORD>(n), &w, nullptr) && w == static_cast<DWORD>(n);
    CloseHandle(f);
    if (!ok) {
        hlog("crash marker: couldn't be written (the layer stays in, with no protection against a crash loop)");
        return false;
    }
    g_marker_state = loading ? 2 : 1;
    hlog("crash marker: %s", loading ? "loading" : "written");
    return true;
}

// ---- the Button prompts row and its ini -------------------------------------------------------------

const char* const kChoices[3] = {"Automatic", "Controller", "Keyboard and mouse"};
fs::path ini_path() { return fh2mods() / L"platform-hudlayer.ini"; }

void read_ini() {
    const fs::path ini = ini_path();
    if (!file_exists(ini)) return;
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"HudLayer", L"ButtonPrompts", L"\x1F", text, 32, ini.c_str());
    if (text[0] == 0x1F && text[1] == 0) return;   // not set
    const wchar_t* t = text;
    while (*t == L' ' || *t == L'\t') ++t;
    const bool number = *t >= L'0' && *t <= L'9';
    const int v       = static_cast<int>(GetPrivateProfileIntW(L"HudLayer", L"ButtonPrompts", 0, ini.c_str()));
    if (!number || v < 0 || v > 2) {
        g_ini_bad = true;
        return;
    }
    g_choice = v;
}

void write_ini(int v) {
    const fs::path ini = ini_path();
    if (!file_exists(ini)) {
        if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
            std::fprintf(f,
                         "; FH2 Mod Platform, HUD layer preview: what the HUD bar's button prompts show.\r\n"
                         "[HudLayer]\r\n"
                         "; 0 Automatic: the controller or the keyboard and mouse, whichever was used last (Xbox-style controllers\r\n"
                         ";   only). 1 Controller: always the controller's buttons. 2 Keyboard and mouse: always key caps.\r\n"
                         "ButtonPrompts=%d\r\n",
                         v);
            std::fclose(f);
        }
        return;
    }
    WritePrivateProfileStringW(L"HudLayer", L"ButtonPrompts", std::to_wstring(v).c_str(), ini.c_str());
}

void on_row(void*, const char*, int value) {
    if (value < 0 || value > 2) return;
    g_choice = value;
    write_ini(value);
    hlog("button prompts: now %s (Mods page)", kChoices[value]);
}

void add_row(FH2Mod* m) {
    FH2Setting s{};
    s.size        = sizeof s;
    s.key         = "HudButtonPrompts";
    s.label       = "Button prompts";
    s.description = "What the HUD bar's button prompts show. Automatic: the controller or the keyboard and mouse, whichever you used last "
                    "(Xbox-style controllers only; choose Controller for others). This is a development build of the FH2 "
                    "Mod Platform.";
    s.kind         = FH2_SETTING_CHOICE;
    s.choices      = kChoices;
    s.choice_count = 3;
    s.value        = g_choice.load();
    s.on_change    = on_row;
    settings_add(m, &s);
}

// ---- the game's Strings and notifies (HudGame) ------------------------------------------------------

// A new AVUI String of `text`: alloc(0x40, the String type), then the UTF-16 constructor, which runs
// Object::Object first, so the String carries one reference. True with *out null if the allocation gave
// nothing (no fault).
bool string_guarded(void** out, const wchar_t* text, int len, DWORD* code) {
    __try {
        void* type = reinterpret_cast<void* (*)(int)>(G(RVA_STRING_TYPE))(0);
        void* s    = reinterpret_cast<void* (*)(uint64_t, void*)>(G(RVA_AVUI_ALLOC))(0x40, type);
        if (!s) return true;
        void* r = reinterpret_cast<void* (*)(void*, const wchar_t*, int)>(G(RVA_STRING_WCTOR))(s, text, len);
        *out    = r ? r : s;
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Release {
    uintptr_t free_fn;
    void* obj;
    bool freed;
};

bool status_guarded(FH2HudStatusFn fn, void* user, uint32_t bits, uint32_t* code) {
    __try {
        fn(user, bits);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct GameLink final : HudGame {
    HudEngine* eng = nullptr;
    bool make_string(const wchar_t* text, int len, void** out) override {
        *out       = nullptr;
        DWORD code = 0;
        if (string_guarded(out, text, len, &code)) return true;
        *out = nullptr;
        hlog("making a String crashed in the game's code (exception 0x%08lX); the HUD layer is off", static_cast<unsigned long>(code));
        return false;
    }
    void add_ref(void* s) override { InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<char*>(s) + 0x10)); }
    bool release(void* s, bool* freed) override {
        Release r{G(RVA_OBJECT_FREE), s, false};
        DWORD code    = 0;
        const bool ok = guarded(
            [](void* p) {
                auto* r = static_cast<Release*>(p);
                if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<char*>(r->obj) + 0x10)) == 0) {
                    r->freed = true;
                    reinterpret_cast<void (*)(void*)>(r->free_fn)(r->obj);
                }
            },
            &r, &code);
        *freed = r.freed;
        if (!ok) hlog("releasing a String crashed in the game's code (exception 0x%08lX); the HUD layer is off", static_cast<unsigned long>(code));
        return ok;
    }
    int refs(void* s) override {
        int32_t r = 0;
        return rd(reinterpret_cast<uintptr_t>(s) + 0x10, r) ? r : 0;
    }
    void pin(void* s) override { InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(static_cast<char*>(s) + 0x10), 1 << 20); }
    bool notify(int prop) override {
        uint8_t* d = prop >= 0 && prop < g_nprops ? g_descs[prop].load() : nullptr;
        if (!d) return true;   // the page never asked for it: nothing binds it
        uint32_t code = 0;
        g_in_notify   = true;
        const bool ok = settings_notify(g_notify_page, d, &code);
        g_in_notify   = false;
        if (!ok)
            hlog("notify crashed in the game's code (exception 0x%08X, page 0x%llX, %s, at frame %llu); the HUD layer is off, and the page "
                 "keeps what it was last told",
                 code, ullp(g_notify_page), eng->props()[static_cast<std::size_t>(prop)].name.c_str(), u64(g_frame.load()));
        return ok;
    }
};

GameLink g_game;
HudEnv make_env() {
    HudEnv e;
    e.game        = &g_game;
    e.clock       = now_ms;
    e.log         = [](const std::string& who, const std::string& text) { log_line(who, text); };
    e.call_status = status_guarded;
    return e;
}
HudEngine g_eng{make_env()};
struct LinkEngine {
    LinkEngine() { g_game.eng = &g_eng; }
} g_link_engine;

// ---- the getter and the descriptors -----------------------------------------------------------------

using BoxBoolFn  = void** (*)(void** out, const bool* v);
using BoxIntFn   = void** (*)(void** out, const int* v);
using BoxFloatFn = void** (*)(void** out, const float* v);

bool box_guarded(void** out, char type, uint32_t bits, DWORD* code) {
    __try {
        if (type == 'b') {
            const bool v = bits != 0;
            reinterpret_cast<BoxBoolFn>(G(RVA_BOX_BOOL))(out, &v);
        } else if (type == 'i') {
            const int v = static_cast<int>(bits);
            reinterpret_cast<BoxIntFn>(G(RVA_BOX_INT))(out, &v);
        } else {
            float v = 0;
            std::memcpy(&v, &bits, 4);
            reinterpret_cast<BoxFloatFn>(G(RVA_BOX_FLOAT))(out, &v);
        }
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int desc_index(void* desc) {
    struct {
        uint32_t magic, pad, index;
    } tag{};
    if (desc && safe_copy(&tag, static_cast<char*>(desc) + 0x7F8, sizeof tag) && tag.magic == kDescMagic &&
        static_cast<int>(tag.index) < g_nprops && g_descs[tag.index].load(std::memory_order_relaxed) == desc)
        return static_cast<int>(tag.index);
    for (int i = 0; i < g_nprops; ++i)
        if (g_descs[i].load(std::memory_order_relaxed) == desc) return i;
    return -1;
}

void** __cdecl hud_get(void** out, void* desc, void* obj) {
    *out        = nullptr;
    const int i = desc ? desc_index(desc) : -1;
    if (i < 0) return out;
    uintptr_t vt   = 0;
    const bool hud = obj && rd(obj, vt) && vt == G(RVA_HUD_VT);
    if (!g_in_notify) getter_seen(obj, vt, hud);
    ++g_getter_calls;
    const HudProp& p = g_eng.props()[static_cast<std::size_t>(i)];
    if (p.type == 's') {
        *out = g_eng.string(i, !hud);
        return out;
    }
    const int kind = p.type == 'b' ? 0 : p.type == 'i' ? 1 : 2;
    if (g_box_off[kind].load()) return out;
    DWORD code = 0;
    if (box_guarded(out, p.type, g_eng.value(i, !hud), &code)) return out;
    *out = nullptr;
    if (!g_box_off[kind].exchange(true)) {
        hlog("the getter's values crashed in the game's code (exception 0x%08lX, %s); the HUD layer is off", static_cast<unsigned long>(code),
             p.name.c_str());
        g_eng.set_off(p.type == 'b' ? HudOff::bool_box_fault : HudOff::value_box_fault);
    }
    return out;
}

// ---- the game's HUD modules and the hold ------------------------------------------------------------

struct Modules {
    uintptr_t wm = 0, world = 0;
    void* mod[2]  = {};   // HUDModule, HUDModuleAR
    void* prio[2] = {};
    bool found    = false;
} g_mods;                 // the pump only
double g_last_walk = -1e9;

// The prioritiser a HUD module holds (module+0x68, its control block +0x70 with uses > 0).
void* prio_of(void* m) {
    uintptr_t ctrl = 0, prio = 0;
    int32_t uses   = 0;
    if (!m || !rd(reinterpret_cast<uintptr_t>(m) + 0x70, ctrl) || !ctrl || !rd(ctrl + 8, uses) || uses <= 0 ||
        !rd(reinterpret_cast<uintptr_t>(m) + 0x68, prio) || !prio || vtable_of(reinterpret_cast<void*>(prio)) != G(RVA_PRIO_VT))
        return nullptr;
    return reinterpret_cast<void*>(prio);
}
int prio_flag(void* prio) {   // prio+0xA8: 1 suspended; -1 unreadable
    uint8_t f = 0;
    return prio && rd(reinterpret_cast<uintptr_t>(prio) + kPrioSuspended, f) ? f : -1;
}

// The world's module list, by exact vtable.
bool walk_modules(Modules& out) {
    out                = Modules{};
    const uintptr_t wm = rdp(G(RVA_WM_PTR));
    if (!wm || vtable_of(reinterpret_cast<void*>(wm)) != G(RVA_WM_VT)) return false;
    const uintptr_t world = rdp(wm + 8), head = world ? rdp(world + 0xF8) : 0;
    if (!world || !head) return false;
    out.wm         = wm;
    out.world      = world;
    uintptr_t node = rdp(head);
    for (int i = 0; i < 512 && node && node != head; ++i) {
        uint64_t used = 0;
        if (rd(node + 0x20, used) && used) {
            const uintptr_t e = rdp(node + 0x18), holder = e ? rdp(e) : 0, obj = holder ? rdp(holder + 0x10) : 0;
            const uintptr_t vt = obj ? vtable_of(reinterpret_cast<void*>(obj)) : 0;
            if (vt && vt == G(RVA_HUDMODULE_VT) && !out.mod[0]) out.mod[0] = reinterpret_cast<void*>(obj);
            if (vt && vt == G(RVA_HUDMODULE_AR_VT) && !out.mod[1]) out.mod[1] = reinterpret_cast<void*>(obj);
        }
        const uintptr_t next = rdp(node);
        if (!next) break;
        node = next;
    }
    for (int k = 0; k < 2; ++k) out.prio[k] = prio_of(out.mod[k]);
    out.found = out.mod[0] || out.mod[1];
    return out.found;
}

struct Hold {
    bool held     = false;
    bool ours[2]  = {};   // the platform's suspend set this module's flag: its resume is the platform's to do
    int reasserts = 0, reasserts_logged = 0, reasserts_since_stats = 0;
    bool waiting_logged = false;
} g_hold;                 // the pump only
// A suspend or a resume that crashed in the game's code turns only itself off for the session: a fault
// in one says nothing about the other, and a module the platform suspended must still be resumed.
bool g_suspend_off = false, g_resume_off = false;   // the pump only
bool g_suspend_off_logged = false, g_resume_off_logged = false;
const char* const kModName[2]      = {"HUDModule", "HUDModuleAR"};
const uintptr_t kSuspendRva[2]     = {RVA_SUSPEND, RVA_SUSPEND_AR};

const char* nth(int r) { return r % 100 >= 11 && r % 100 <= 13 ? "th" : r % 10 == 1 ? "st" : r % 10 == 2 ? "nd" : r % 10 == 3 ? "rd" : "th"; }

void hold() {
    if (g_suspend_off) {
        if (!g_suspend_off_logged) hlog("hold: the suspend crashed earlier this session; the game's HUD stays");
        g_suspend_off_logged = true;
        return;
    }
    // a hold now could never be let go: release() makes no more resumes after one crashed
    if (g_resume_off) {
        if (!g_resume_off_logged) hlog("hold: the resume crashed earlier this session, so a hold couldn't be let go; the game's HUD stays");
        g_resume_off_logged = true;
        return;
    }
    if (!g_mods.found) {
        if (!g_hold.waiting_logged) hlog("hold: waiting for a world to be loaded");
        g_hold.waiting_logged = true;
        return;
    }
    g_hold.waiting_logged = false;
    // both flags first: the two modules may share one prioritiser
    int before[2] = {prio_flag(g_mods.prio[0]), prio_flag(g_mods.prio[1])};
    for (int k = 0; k < 2; ++k) {
        if (!g_mods.prio[k] || before[k] != 0) continue;
        DWORD code = 0;
        if (call1(kSuspendRva[k], g_mods.mod[k], &code)) {
            g_hold.ours[k] = true;
            continue;
        }
        // it crashed: no more suspends, and hold_game_hud says 0 from now on. If it got as far as the flag, the
        // resume is still the platform's: the next frame lets the hold go (no wish counts any more).
        g_suspend_off = true;
        g_eng.set_hold_available(false);
        g_hold.ours[k] = prio_flag(g_mods.prio[k]) == 1;
        hlog("hold: the %s suspend crashed (exception 0x%08lX); its flag reads %d, so it's %s", kModName[k], static_cast<unsigned long>(code),
             prio_flag(g_mods.prio[k]), g_hold.ours[k] ? "resumed when the hold is let go" : "left alone");
        break;
    }
    const int after[2] = {prio_flag(g_mods.prio[0]), prio_flag(g_mods.prio[1])};
    g_hold.held        = true;
    g_hold.reasserts = g_hold.reasserts_logged = g_hold.reasserts_since_stats = 0;
    hlog("hold: the game's HUD is hidden (HUDModule flag %d -> %d, HUDModuleAR flag %d -> %d, one prioritiser: %s) for %s", before[0], after[0],
         before[1], after[1], g_mods.prio[0] && g_mods.prio[0] == g_mods.prio[1] ? "yes" : "no", g_eng.holders().c_str());
}

// Each frame while held: the game shows its HUD again by itself at times (after a menu, as driving
// starts), and another mod may let go of a suspend of its own. Both flags are read before any suspend,
// as hold() does: with one prioritiser for both modules, the HUDModule's suspend sets the flag the AR
// module's would read, and the AR suspend, whose hide pass is the only one that hides the statuses the
// AR module owns, would never run again. Every module found whose flag reads 0 is suspended again and is
// the platform's to resume from then on.
void hold_tick() {
    if (!g_hold.held || g_suspend_off || g_resume_off) return;
    int before[2] = {-1, -1};
    for (int k = 0; k < 2; ++k)
        if (g_mods.prio[k]) before[k] = prio_flag(g_mods.prio[k]);
    if (before[0] != 0 && before[1] != 0) return;
    ++g_hold.reasserts;
    ++g_hold.reasserts_since_stats;
    for (int k = 0; k < 2; ++k) {
        if (before[k] != 0) continue;
        DWORD code = 0;
        if (!call1(kSuspendRva[k], g_mods.mod[k], &code)) {
            g_suspend_off = true;
            g_eng.set_hold_available(false);   // hold_game_hud says 0 from now on; the next frame lets the hold go
            hlog("hold: suspending the %s again crashed; the game's HUD isn't hidden again this session (the hold is still let go as usual)",
                 kModName[k]);
            return;
        }
        g_hold.ours[k] = true;
    }
    if (g_hold.reasserts_logged < 5) {
        ++g_hold.reasserts_logged;
        hlog("hold: the game showed its HUD again at frame %llu; hidden again (%d%s time)", u64(g_frame.load()), g_hold.reasserts,
             nth(g_hold.reasserts));
    }
}

// The cached WorldManager, world and modules are still the game's current ones.
bool modules_current() {
    if (!g_mods.found) return false;
    const uintptr_t wm = rdp(G(RVA_WM_PTR)), world = wm ? rdp(wm + 8) : 0;
    if (wm != g_mods.wm || world != g_mods.world) return false;
    static const uintptr_t kVt[2] = {RVA_HUDMODULE_VT, RVA_HUDMODULE_AR_VT};
    for (int k = 0; k < 2; ++k)
        if (g_mods.mod[k] && (vtable_of(g_mods.mod[k]) != G(kVt[k]) || prio_of(g_mods.mod[k]) != g_mods.prio[k])) return false;
    return true;
}

// Resumes each module the platform suspended whose flag still reads 1, if the modules are still the
// game's current ones (stale ones are forgotten). Tried even after a suspend crashed; only a resume that
// crashed stops resumes, and holds. The resume clears the flag first (0x140785CE7) and only then releases
// its shared_ptr, so a resume that crashed is read again: a flag of 0 means the game's HUD is back.
void release(bool world_gone) {
    if (!g_hold.held) return;
    world_gone = world_gone || !modules_current();
    std::string kept;                // modules that may still be suspended, with why
    const char* cleared = nullptr;   // a module whose resume crashed after clearing the flag (one at most)
    for (int k = 0; k < 2; ++k) {
        if (!g_hold.ours[k]) continue;
        g_hold.ours[k] = false;
        if (world_gone || !g_mods.mod[k]) continue;
        const int flag = prio_flag(g_mods.prio[k]);
        if (flag == 0) continue;   // shown already: a shared prioritiser resumed above, or the game showed it
        const char* why = nullptr;
        DWORD code      = 0;
        if (flag < 0) {
            why = "its flag can't be read";
        } else if (g_resume_off) {
            why = "the resume crashed earlier this session";
        } else if (!call1(RVA_RESUME, g_mods.mod[k], &code)) {
            g_resume_off = true;
            g_eng.set_hold_available(false);   // hold_game_hud says 0 from now on
            if (prio_flag(g_mods.prio[k]) == 0) cleared = kModName[k];
            else why = "the resume crashed";
        } else if (prio_flag(g_mods.prio[k]) != 0) {
            why = "its flag doesn't read 0 after the resume";
        }
        if (why) kept += std::string(kept.empty() ? "" : "; ") + kModName[k] + " not resumed: " + why;
    }
    g_hold.held = false;
    const std::string after = !cleared ? std::string()
                                       : std::string("the ") + cleared + "'s resume crashed after it cleared the flag (it reads 0), so " +
                                             (kept.empty() ? "the game's HUD is back" : "that module is resumed") + "; no more holds this session";
    if (world_gone) hlog("hold: let go (the world changed: nothing to resume)");
    else if (!kept.empty()) hlog("hold: not let go: %s%s%s; the game's HUD may stay hidden", after.c_str(), after.empty() ? "" : "; ", kept.c_str());
    else if (!after.empty()) hlog("hold: let go: %s", after.c_str());
    else hlog("hold: let go");
}

// Each frame: the modules, cached; any change once found is "the world changed". With none cached, a
// walk once a second.
void modules_tick(double now) {
    if (g_mods.found) {
        const uintptr_t wm = rdp(G(RVA_WM_PTR)), world = wm ? rdp(wm + 8) : 0;
        bool changed                  = wm != g_mods.wm || world != g_mods.world;
        static const uintptr_t kVt[2] = {RVA_HUDMODULE_VT, RVA_HUDMODULE_AR_VT};
        for (int k = 0; k < 2 && !changed; ++k)
            if (g_mods.mod[k] && (vtable_of(g_mods.mod[k]) != G(kVt[k]) || prio_of(g_mods.mod[k]) != g_mods.prio[k])) changed = true;
        if (!changed) return;
        hlog("modules: the world changed (WorldManager 0x%llX, world 0x%llX)", ull(wm), ull(world));
        release(true);   // forgotten without calls; every wish stays
        g_mods      = Modules{};
        g_last_walk = now;
        return;
    }
    if (now - g_last_walk < 1000) return;
    g_last_walk = now;
    Modules m;
    if (!walk_modules(m)) return;
    g_mods = m;
    hlog("modules: HUDModule 0x%llX (prioritiser 0x%llX), HUDModuleAR 0x%llX (prioritiser 0x%llX), one prioritiser: %s", ullp(m.mod[0]),
         ullp(m.prio[0]), ullp(m.mod[1]), ullp(m.prio[1]), m.prio[0] && m.prio[0] == m.prio[1] ? "yes" : "no");
}

bool game_hidden() {
    return g_hold.held && (prio_flag(g_mods.prio[0]) == 1 || prio_flag(g_mods.prio[1]) == 1);
}

// The UI lock's owner token: [[[game+0x36B0418]+0x30]+0x160]. 0 when free.
bool ui_lock_owner(uint64_t* owner) {
    const uintptr_t a = rdp(G(RVA_UI_GLOBAL)), b = a ? rdp(a + 0x30) : 0;
    return b && rd(b + 0x160, *owner);
}

// ---- UI sounds --------------------------------------------------------------------------------------

void play(int sound, int layer) {
    const char* name = HudEngine::sound_name(static_cast<uint32_t>(sound));
    const Sound& s   = kSounds[sound - FH2_SOUND_ERROR];
    if (!settings_sounds_ok()) {
        if (g_eng.once(-1, "sound player")) hlog("sound: %s not played: the platform's sound player isn't ready (the MODS tab's hooks didn't go in)", name);
        return;
    }
    const uintptr_t player = rdp(G(RVA_SOUND_PLAYER)), vt = player ? vtable_of(reinterpret_cast<void*>(player)) : 0,
                    slot = vt ? rdp(vt + 8) : 0;
    if (!player || vt != G(RVA_SOUND_PLAYER_VT) || slot != G(RVA_SOUND_PLAY)) {
        if (g_eng.once(-1, "sound player shape"))
            hlog("sound: %s not played: the sound player isn't as expected (0x%llX, vtable 0x%llX, slot +8 0x%llX)", name, ull(player), ull(vt),
                 ull(slot));
        return;
    }
    char got[96]        = {};
    const std::size_t n = std::strlen(s.text) + 1;
    const uintptr_t at  = rdp(G(s.slot));
    if (at != G(s.text_rva) || !safe_copy(got, reinterpret_cast<const void*>(at), n) || std::memcmp(got, s.text, n) != 0) {
        if (g_eng.once(-1, std::string("sound slot ") + name)) hlog("sound: %s not played: its name slot doesn't hold the expected text", name);
        return;
    }
    uint32_t code = 0;
    if (settings_play_sound(s.text, &code)) {
        g_eng.log_rated(layer, 0, std::string("sound: ") + name + " played");
        return;
    }
    if (!code) {
        if (g_eng.once(-1, "sound player")) hlog("sound: %s not played: the platform's sound player isn't ready", name);
        return;
    }
    g_eng.set_sounds_available(false);
    hlog("sound: %s crashed in the game's code (exception 0x%08X); sounds are off", name, code);
}

// ---- the crash text and the stats -------------------------------------------------------------------

char g_crash[2][512];
std::atomic<int> g_crash_ix{-1};

const char* marker_word() {
    switch (g_marker_state.load()) {
        case 1: return "written";
        case 2: return "loading";
        case 3: return "cleared";
        default: return "none";
    }
}

// Rewritten when anything in it but the frame changed, and every 10 frames: the buffer not in use, then
// the index flipped to it, so a crash in the middle of a rewrite still reads a whole line.
struct CrashKey {
    void* page;
    bool on_screen, live, held;
    int owner, marker;
    uint32_t parts;
    uint64_t notify;
    std::size_t strings;
    bool operator==(const CrashKey& o) const {
        return page == o.page && on_screen == o.on_screen && live == o.live && held == o.held && owner == o.owner && marker == o.marker &&
               parts == o.parts && notify == o.notify && strings == o.strings;
    }
};
CrashKey g_crash_key{};

void crash_text(bool live) {
    const int own = g_eng.owner();
    const CrashKey key{g_page.load(), g_on_screen.load(), live, g_hold.held, own, g_marker_state.load(), g_eng.shown_parts(),
                       g_eng.last_notify_frame(), g_eng.strings_in_use()};
    if (g_crash_ix.load() >= 0 && key == g_crash_key && g_frame.load() % 10 != 0) return;
    g_crash_key  = key;
    const int ix = g_crash_ix.load() == 0 ? 1 : 0;
    std::snprintf(g_crash[ix], sizeof g_crash[ix],
                  "marker %s; page 0x%llX (on screen %s, live %s); owner %s; parts %s; hold %s; frame %llu; last notify frame %llu; strings %zu",
                  marker_word(), ullp(g_page.load()), g_on_screen.load() ? "yes" : "no", live ? "yes" : "no",
                  own >= 0 ? g_eng.layer_name(own).c_str() : "none", hud_parts_text(g_eng.shown_parts()).c_str(),
                  g_hold.held ? "held" : "not held", u64(g_frame.load()), u64(g_eng.last_notify_frame()), g_eng.strings_in_use());
    g_crash_ix = ix;
}

struct PumpCost {
    uint64_t frames = 0;
    double work = 0, work_max = 0;
    uint64_t getters0 = 0;
} g_cost;
double g_last_stats = 0, g_stats_from = 0;
int g_reasserts_all = 0;

void stats(double now, bool live) {
    const HudEngine::Stats s = g_eng.take_stats();
    const PumpCost& c        = g_cost;
    const double f           = c.frames ? static_cast<double>(c.frames) : 1.0;
    const int own            = g_eng.owner();
    const uint64_t getters   = g_getter_calls.load();
    hlog("stats: %llu frames in %.1f s; pump avg %.3f ms, max %.3f ms; notifies %.1f a frame (max %llu), %llu carried over by the budget; "
         "getter calls %llu; staged copies skipped while busy %llu; on screen: %s; owner %s; parts %s",
         u64(c.frames), (now - g_stats_from) / 1000.0, c.work / f, c.work_max, static_cast<double>(s.notifies) / f, u64(s.max_notifies),
         u64(s.carried), u64(getters - c.getters0), u64(s.skipped), live ? "yes" : "no", own >= 0 ? g_eng.layer_name(own).c_str() : "none",
         hud_parts_text(g_eng.shown_parts()).c_str());
    hlog("%s", g_eng.strings_line().c_str());
    hlog("threads: pump %s, shown %s, hidden %s, destroyed %s, getters on screen %s (%llu calls), getters while building %s (%llu calls)",
         thread_text(g_th_pump).c_str(), thread_text(g_th_shown).c_str(), thread_text(g_th_hidden).c_str(), thread_text(g_th_dtor).c_str(),
         thread_text(g_th_get_screen).c_str(), u64(g_get_screen.load()), thread_text(g_th_get_build).c_str(), u64(g_get_build.load()));
    {
        std::scoped_lock lk{g_seen_mu};
        for (const SeenPage& p : g_seen_pages)
            if (p.refusals) {
                char b[48];
                hlog("gate: page 0x%llX (%s) got defaults %llu time(s)", ullp(p.page), page_class(p.vt, b, sizeof b), u64(p.refusals));
            }
    }
    if (g_hold.reasserts_since_stats) {
        g_reasserts_all += g_hold.reasserts_since_stats;
        hlog("hold: the game showed its HUD again %d time(s) since the last stats (%d in all)", g_hold.reasserts_since_stats, g_reasserts_all);
        g_hold.reasserts_since_stats = 0;
    }
    g_cost          = PumpCost{};
    g_cost.getters0 = getters;
    g_stats_from    = now;
}

// ---- the pump ---------------------------------------------------------------------------------------

bool g_lock_free_logged = false, g_interlock_logged = false;

void pump(double t0) {
    const uint64_t frame = g_frame.load();
    // the interlock: the page's shown and its getters on the pump's thread, or the layer goes off
    if (!g_eng.off() && !interlock_ok() && (g_last_shown_thread.load() || g_th_get_screen.id.load())) {
        if (!g_interlock_logged)
            hlog("the HUD layer's updates are on another thread than the HUD page (pump %lu, shown %lu, getters %s); the HUD layer is off",
                 static_cast<unsigned long>(g_th_pump.id.load()), static_cast<unsigned long>(g_last_shown_thread.load()),
                 thread_text(g_th_get_screen).c_str());
        g_interlock_logged = true;
        g_eng.set_off(HudOff::interlock);
    }
    const int choice = g_choice.load();
    const bool kbd   = choice == 1 ? false : choice == 2 ? true : device_auto_kbd() != 0;
    if (g_hold_code_ok.load()) modules_tick(t0);
    void* page      = g_page.load();
    const bool live = page && g_on_screen.load() && page_is_hud(page) && interlock_ok();
    g_notify_page   = page;
    // the hold, counted across mods
    if (g_hold_code_ok.load()) {
        const bool wanted = g_eng.hold_wanted();
        if (wanted && !g_hold.held) hold();
        else if (!wanted && g_hold.held) release(false);
        hold_tick();
    }
    HudEngine::FrameIn in;
    in.now           = t0;
    in.frame         = frame;
    in.live          = live;
    in.page          = page;
    in.page_gen      = g_page_gen.load();
    in.kbd           = kbd;
    in.device_forced = choice != 0;
    in.available     = !g_seen_shown.load() || g_page_read.load();
    in.game_hidden   = game_hidden();
    const HudEngine::FrameOut out = g_eng.frame(in);
    if (out.sound >= 0) play(out.sound, out.sound_layer);
    // the crash marker, 10 s after the page was first on screen
    const double first = g_first_on_screen.load();
    if (first > 0 && t0 - first >= 10000 && g_marker_state.load() != 0 && !g_marker_cleared.exchange(true)) {
        DeleteFileW(marker_path().c_str());
        g_marker_state = 3;
        hlog("crash marker: cleared: the HUD page was on screen for 10 s");
    }
    crash_text(live);
    const double t1 = now_ms(), work = t1 - t0;
    ++g_cost.frames;
    g_cost.work += work;
    g_cost.work_max = std::max(g_cost.work_max, work);
    if (g_last_stats == 0) g_last_stats = g_stats_from = t0;
    if (t1 - g_last_stats >= 10000) {
        g_last_stats = t1;
        stats(t1, live);
    }
}

void on_pump(FH2Context*, void*) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    const double t0 = now_ms();
    const uint64_t frame = ++g_frame;
    g_th_pump.see(GetCurrentThreadId());
    g_last_pump = t0;
    uint64_t owner = 0;
    if (ui_lock_owner(&owner) && owner == 0) {   // no game calls on such a frame
        if (!g_lock_free_logged)
            hlog("the UI lock reads free at the pump (frame %llu); the HUD layer makes no game calls on such frames", u64(frame));
        g_lock_free_logged = true;
        return;
    }
    pump(t0);
}

// ---- the page's hooks -------------------------------------------------------------------------------

using HudFn = uint64_t (*)(void*, void*, void*, void*);
HudFn o_layer_shown = nullptr, o_layer_hidden = nullptr, o_layer_dtor = nullptr;
std::atomic<bool> g_other_shown_logged{false};

uint64_t d_layer_shown(void* page, void* b, void* c, void* d) {
    const uint64_t r = o_layer_shown(page, b, c, d);
    if (!page_is_hud(page)) {
        if (page && !g_other_shown_logged.exchange(true)) {
            char buf[48];
            hlog("shown: 0x%llX (%s) isn't the Hud class; not tracked", ullp(page), page_class(vtable_of(page), buf, sizeof buf));
        }
        return r;
    }
    const DWORD tid = GetCurrentThreadId();
    g_th_shown.see(tid);
    g_last_shown_thread = tid;
    void* old           = g_page.exchange(page);
    if (old != page) ++g_page_gen;
    g_on_screen         = true;
    g_seen_shown        = true;
    if (g_first_on_screen.load() == 0) g_first_on_screen = now_ms();
    if (old && old != page) hlog("a new HUD page is tracked (the last one was 0x%llX)", ullp(old));
    hlog("HUD page 0x%llX (Hud) shown at frame %llu (thread %lu)", ullp(page), u64(g_frame.load()), static_cast<unsigned long>(tid));
    if (!g_page_read.load() && !g_eng.off()) {
        hlog("the HUD page was shown without the layer's values, so the page the game built doesn't have the layer (is mediaoverride off?); "
             "the HUD layer is off");
        g_eng.set_off(HudOff::page_unread);
    }
    return r;
}

uint64_t d_layer_hidden(void* page, void* b, void* c, void* d) {
    const uint64_t r = o_layer_hidden(page, b, c, d);
    if (page && page == g_page.load()) {
        g_th_hidden.see(GetCurrentThreadId());
        g_on_screen = false;
        hlog("HUD page 0x%llX (Hud) hidden at frame %llu", ullp(page), u64(g_frame.load()));
    }
    return r;
}

uint64_t d_layer_dtor(void* page, void* flags, void* c, void* d) {
    if (page) {
        // any Hud page, tracked or not, is forgotten by the getters' bookkeeping before the original frees
        // it, so a new page at the same address is seen afresh
        {
            std::scoped_lock lk{g_seen_mu};
            g_seen_pages.erase(std::remove_if(g_seen_pages.begin(), g_seen_pages.end(), [&](const SeenPage& s) { return s.page == page; }),
                               g_seen_pages.end());
        }
        void* last = page;
        g_last_getter_obj.compare_exchange_strong(last, nullptr);
    }
    void* tracked = page;
    if (page && g_page.compare_exchange_strong(tracked, nullptr)) {
        // forgotten first, before the original frees it
        const DWORD tid = GetCurrentThreadId();
        g_th_dtor.see(tid);
        ++g_page_gen;
        g_on_screen = false;
        hlog("HUD page 0x%llX (Hud) destroyed at frame %llu (thread %lu)", ullp(page), u64(g_frame.load()), static_cast<unsigned long>(tid));
    }
    return o_layer_dtor(page, flags, c, d);
}

// ---- start-up ---------------------------------------------------------------------------------------

void start_thread() {
    void* base = g_api->wait_for_code(0xFFFFFFFFu);
    if (!base) {
        hlog("the game's code never became ready (or Azimuth started another game); no HUD layer");
        g_eng.set_off(HudOff::left_out);
        return;
    }
    g_base = reinterpret_cast<uintptr_t>(base);
    const char* const kOff = "the HUD layer is off";
    // the code checks, on disk, before any hook; with no exe to read, nothing can be checked (one line, not
    // a "doesn't match" for every group)
    if (!disk_image_readable()) {
        hlog("the HUD layer is off: the game exe couldn't be read from the game folder, so the layer's game code can't be checked");
        g_eng.set_off(HudOff::left_out);
        return;
    }
    struct Group {
        const char* name;
        bool ok;
    };
    Group required[4] = {
        {"the pump", check_group("the pump", kOff,
                                 {
                                     B(RVA_TICK_FN, "40 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF"),
                                     B(RVA_TICK, "8B 81 A8 00 00 00"),
                                 })},
        {"the UI lock", check_group("the UI lock", kOff,
                                    {
                                        B(0x2269E6, "FF 52 30"),
                                        T(0x2269FA, 0x14071BDC0),
                                        T(0x226A46, 0x14071BD60),
                                        B(0x227045, "FF 52 40"),
                                        B(0x15E63A0, "48 89 5C 24 08 57 48 83 EC 20 0F B6 FA 48 8B D9 E8 7B 4E 0F 00 48 39 03 74 33"),
                                        B(0x159C909, "48 8B 0D 08 3B 11 02"),   // [game+0x36B0418]
                                        B(0x159C918, "48 8D 88 60 01 00 00"),   // +0x160
                                        B(0x14A84E0, "48 8B 41 30 C3"),         // +0x30
                                        B(0x16024D6, "48 C7 01 00 00 00 00"),   // the owner token cleared on unlock
                                    })},
        {"the HUD page", check_group("the HUD page", kOff,
                                     {
                                         Q(RVA_HUD_VT, kImageBase + RVA_HUD_DTOR),
                                         B(RVA_HUD_DTOR, "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE"),
                                         Q(RVA_HUD_SHOWN_SLOT, kImageBase + RVA_HUD_SHOWN),
                                         B(RVA_HUD_SHOWN, "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 E0"),
                                         B(0xD38DF2, "C6 81 10 04 00 00 01"),   // shown: page+0x410 = 1
                                         Q(RVA_HUD_HIDDEN_SLOT, kImageBase + RVA_HUD_HIDDEN),
                                         B(RVA_HUD_HIDDEN, "48 8B C4 55 48 8D 68 A1 48 81 EC F0 00 00 00 48"),
                                         B(0xD4156B, "C6 87 10 04 00 00 00"),   // hidden: page+0x410 = 0
                                     })},
        {"Strings", check_group("Strings", kOff,
                                {
                                    B(RVA_AVUI_ALLOC, "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 6F 97 26"),     // alloc(size, type)
                                    B(RVA_STRING_WCTOR, "48 89 4C 24 08 57 41 56 41 57 48 83 EC 30 48 C7"),   // String(obj, const wchar_t*, len)
                                    B(0x166336F, "E8 0C B8 DD FF"),                                          // ... Object::Object first (refcount 1)
                                    B(0x143EB9D, "C7 41 10 01 00 00 00"),                                    // Object::Object: refcount +0x10 = 1
                                    Q(0x2D5CAA0, 0x140AFF330),                                               // String vtable slot 0 (deleting dtor)
                                    B(0xAFF330, "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 5C 5D B6 00"),
                                    B(0x16650A0, "48 89 4C 24 08 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 48 8B D9"),   // ~String
                                    B(0x144CA70, "48 8B 05 F1 F1 25 02 48 85 C0 74 03 48 FF E0 48"),         // the paired free
                                    B(RVA_STRING_TYPE, "48 8D 05 59 B0 02 02 C3"),                           // the String type
                                    B(RVA_OBJECT_FREE, "40 53 48 83 EC 20 48 8B D9 E8 B2 A2 1F 01 48 85 DB 74 13 48 8B 03 BA 01"),   // free at 0
                                })},
    };
    const bool hold_ok = check_group("the game-HUD hold", "hold_game_hud does nothing",
                                     {
                                         B(0x921215, "48 8B 0D 24 FD B9 02"),   // reads the WorldManager pointer game+0x34C0F40
                                         B(0x91A4A0, "40 53 48 83 EC 30 48 8B 49 08 48 8B DA C7 44 24 20 00 00 00 00 E8 B6 FF"),
                                         B(0x91A476, "48 81 C1 F8 00 00 00"),
                                         Q(0x2CDC628, 0x14077B800),   // the prioritiser's +0x40 (update)
                                         Q(0x2CDC5F8, 0x1407813D0),   // its +0x10, which clears the suspend
                                         B(0x7813E5, "C6 81 A8 00 00 00 00"),
                                         B(RVA_SUSPEND, "40 57 48 83 EC 50 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 48 89 74 24"),
                                         B(RVA_SUSPEND_AR, "40 57 48 83 EC 50 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 48 89 74 24"),
                                         B(0x78B161, "C6 81 A8 00 00 00 01"),   // HUDModule's suspend sets the flag
                                         B(0x78B211, "C6 81 A8 00 00 00 01"),   // HUDModuleAR's
                                         B(RVA_RESUME, "40 53 48 83 EC 30 48 8D 54 24 20 48 83 C1 50 E8 5C 59 C6 FF 48 8B 08 C6"),
                                         B(0x785CE7, "C6 81 A8 00 00 00 00"),   // the resume clears the flag before its shared_ptr goes
                                     });
    std::vector<Entry> snd = {
        Q(RVA_SOUND_PLAYER_VT + 8, kImageBase + RVA_SOUND_PLAY),
        B(RVA_SOUND_PLAY, "48 89 54 24 10 55 56 57 48 83 EC 60 48 C7 44 24 30 FE FF FF FF"),
        B(0x22C300, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8"),   // std::string::assign (play_sound)
        B(0x1412FE0, "48 8B 05 A9 1B 27 02 C3"),                                 // UserInterface = [0x143684B90]
        B(0xF6A0DD, "48 8B 15 B4 01 6F 02"),                                     // rdx = the player
        B(0xF6A0EA, "41 FF 90 40 02 00 00"),                                     // UserInterface+0x20 = it
    };
    for (const Sound& s : kSounds) {
        snd.push_back(Q(s.slot, kImageBase + s.text_rva));
        snd.push_back(S(s.text_rva, s.text));
    }
    const bool sounds_ok = check_group("UI sounds", "play_ui_sound plays nothing", snd);
    g_hold_code_ok       = hold_ok;
    g_eng.set_hold_available(hold_ok);
    g_eng.set_sounds_available(sounds_ok);
    for (const Group& g : required)
        if (!g.ok) {
            hlog("the HUD layer is off: %s doesn't match", g.name);
            g_eng.set_off(HudOff::left_out);
            return;
        }
    g_code_ok.store(true, std::memory_order_release);   // from now on the getter answers
    // the four hooks, all or none
    struct H {
        const char* name;
        int kind;   // 0 instruction, 1 pointer
        uintptr_t rva;
        void* fn;
        void** original;
    };
    const H hs[4] = {
        {"AchievementsModule update (the HUD layer's pump)", 0, RVA_TICK, reinterpret_cast<void*>(&on_pump), nullptr},
        {"freeroam HUD: shown (HUD layer)", 1, RVA_HUD_SHOWN_SLOT, reinterpret_cast<void*>(&d_layer_shown), reinterpret_cast<void**>(&o_layer_shown)},
        {"freeroam HUD: hidden (HUD layer)", 1, RVA_HUD_HIDDEN_SLOT, reinterpret_cast<void*>(&d_layer_hidden),
         reinterpret_cast<void**>(&o_layer_hidden)},
        {"freeroam HUD: deleting destructor (HUD layer)", 1, RVA_HUD_VT, reinterpret_cast<void*>(&d_layer_dtor),
         reinterpret_cast<void**>(&o_layer_dtor)},
    };
    FH2Hook* made[4] = {};
    for (int i = 0; i < 4; ++i)
        made[i] = hs[i].kind == 0 ? g_api->hook_instruction(&g_layer_mod, hs[i].name, GP(hs[i].rva), reinterpret_cast<FH2InstructionFn>(hs[i].fn), nullptr)
                                  : g_api->hook_pointer(&g_layer_mod, hs[i].name, static_cast<void**>(GP(hs[i].rva)), hs[i].fn, hs[i].original);
    bool all = made[0] && made[1] && made[2] && made[3];
    if (all && !g_api->enable_hooks(&g_layer_mod, nullptr)) {
        // which ones: each alone, then none
        g_api->disable_hooks(&g_layer_mod, nullptr);
        for (int i = 0; i < 4; ++i) {
            if (!g_api->enable_hooks(&g_layer_mod, made[i])) made[i] = nullptr;
            g_api->disable_hooks(&g_layer_mod, nullptr);
        }
        all = false;
    }
    if (!all) {
        g_api->disable_hooks(&g_layer_mod, nullptr);
        for (int i = 0; i < 4; ++i)
            if (!made[i]) hlog("hooks: %s refused (see the line above); the HUD layer is off", hs[i].name);
        g_eng.set_off(HudOff::left_out);
        return;
    }
    // the getter needs FindProperty hooked (settings.cpp), and the float descriptors its sliders' code
    int state = 0;
    for (int i = 0; i < 1200 && !(state = settings_properties_state()); ++i) Sleep(100);
    if (state != 1) {
        hlog("the Mods page's hooks didn't go in, so the game can't read the layer's values; the HUD layer is off");
        g_api->disable_hooks(&g_layer_mod, nullptr);
        g_eng.set_off(HudOff::left_out);
        return;
    }
    g_ready.store(true, std::memory_order_release);   // the pump does nothing before this
    hlog("hooks: 4 of 4 made and on; the HUD layer is ready");
}

// ---- the interface (fh2mod_hud.h) -------------------------------------------------------------------

int i_add_hud_layer(FH2Mod* m, const FH2HudLayer* layer) {
    if (!m) return 0;
    if (!in_mod_init()) {
        log_line(m->name, "add_hud_layer: it was called after fh2mod_init: the HUD page is written once every mod's fh2mod_init has returned; "
                          "no HUD layer");
        return 0;
    }
    const int i = g_eng.add_layer(m, m->name, layer);
    if (i < 0) return 0;
    if (i == 0) g_layer_mod.dll = m->dll;   // the title notice goes when that mod is removed
    // the Button prompts row: in the part of the first mod whose layer shows prompts (the bar or the tip); a mod
    // with only the feed or other parts without prompts doesn't get it
    static bool row_added = false;
    if (!row_added && (g_eng.layer_parts(i) & (FH2_HUD_PART_BAR | FH2_HUD_PART_TIP))) {
        row_added = true;
        read_ini();
        add_row(m);
    }
    const uint32_t lease = g_eng.layer_lease(i);
    log_line(m->name, "HUD layer added: " + hud_parts_text(g_eng.layer_parts(i)) + "; lease " + std::to_string(lease) + " ms" +
                          (layer->lease_ms == 0 ? " (the default)" : ""));
    return 1;
}
int i_set_hud_state(FH2Mod* m, const FH2HudState* s) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.set_state(i, s);
}
int i_set_hud_value(FH2Mod* m, int which, float v) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.set_value(i, which, v);
}
int i_show_hud_layer(FH2Mod* m, uint32_t parts) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.show(i, parts);
}
int i_pulse_hud(FH2Mod* m, int pulse) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.pulse(i, pulse);
}
uint32_t i_hud_status(FH2Mod* m) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.status(i);
}
int i_hold_game_hud(FH2Mod* m, int hold) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.hold(i, hold);
}
int i_play_ui_sound(FH2Mod* m, uint32_t sound) {
    const int i = g_eng.layer_of(m);
    return i < 0 ? 0 : g_eng.play_sound(i, sound);
}
int i_post_hud_feed(FH2Mod* m, const FH2HudFeedLine* line) {
    const int i = g_eng.layer_of(m);
    if (i < 0 && m) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true)) log_line(m->name, "post_hud_feed: this mod has no HUD layer; add one with FH2_HUD_PART_FEED in fh2mod_init");
    }
    return i < 0 ? 0 : g_eng.post_feed(i, line);
}

const FH2HudInterface g_iface = {sizeof(FH2HudInterface), FH2_HUD_INTERFACE_VERSION, i_add_hud_layer, i_set_hud_state, i_set_hud_value,
                                 i_show_hud_layer,        i_pulse_hud,               i_hud_status,    i_hold_game_hud, i_play_ui_sound,
                                 i_post_hud_feed};

std::atomic<bool> g_pump_late_logged{false};

} // namespace

// ---- the platform's calls (hud_layer.hpp) -----------------------------------------------------------

const void* hud_interface() { return &g_iface; }
FH2Mod* hud_layer_owner() { return &g_layer_mod; }
bool hud_layer_active() { return g_active.load(); }
int hud_layer_choice() { return g_choice.load(); }
double hud_layer_now() { return now_ms(); }

void hud_layer_prepare(const FH2ModApi* api) {
    g_api = api;
    if (g_eng.layer_count() == 0) {
        // no layer: as 2.8.0, apart from what an earlier preview session left
        if (file_exists(marker_path())) DeleteFileW(marker_path().c_str());
        if (file_exists(fh2mods() / L"notices" / L"HUD layer.txt")) api->set_notice(&g_layer_mod, "");
        return;
    }
    g_active   = true;
    g_left_out = check_marker();
    g_parts    = g_eng.parts();
    g_eng.build(hud_build_props(g_parts));
    const std::vector<HudProp>& props = g_eng.props();
    g_nprops = static_cast<int>(props.size());
    g_descs.reset(new std::atomic<uint8_t*>[static_cast<std::size_t>(g_nprops)]);
    for (int i = 0; i < g_nprops; ++i) {
        g_descs[i] = nullptr;
        g_names[widen(props[static_cast<std::size_t>(i)].short_name)] = i;
    }
    g_shown_ix = g_eng.prop_index("Shown");
    const std::string table = hud_props_text(props);
    if (FILE* f = _wfopen((fh2mods() / L"platform-hudlayer-props.txt").c_str(), L"wb")) {
        std::fwrite(table.data(), 1, table.size(), f);
        std::fclose(f);
    }
    std::string mods;
    for (int i = 0; i < g_eng.layer_count(); ++i) mods += (i ? ", " : "") + g_eng.layer_name(i);
    hlog("%d mod(s) asked for a HUD layer: %s; the HUD page gets %s", g_eng.layer_count(), mods.c_str(), hud_parts_text(g_parts).c_str());
    if (g_ini_bad) hlog("button prompts: ButtonPrompts isn't 0, 1 or 2; Automatic used");
    else hlog("button prompts: %s (fh2-mods\\platform-hudlayer.ini)", kChoices[g_choice.load()]);
    if (g_left_out) {
        g_eng.set_off(HudOff::left_out);
        api->set_notice(&g_layer_mod, "HUD layer: left out after a crash. See platform.log.");
        return;
    }
    api->set_notice(&g_layer_mod, "");   // a notice from an earlier left-out session goes (g_layer_mod never registers)
}

bool hud_layer_wants_page() { return g_active.load() && !g_left_out && !game_mount().empty(); }

bool hud_layer_splice(std::string& page) {
    if (!hud_layer_wants_page()) return false;
    HudUi ui;
    ui.read     = [](const char* name) { return settings_read_ui(name); };
    ui.balanced = [](const std::string& xml, std::string& why) { return settings_balanced(xml, why); };
    std::string why;
    if (!hud_splice(page, g_parts, g_eng.props(), ui, g_check, why)) {
        hlog("page: the layer is left out: %s", why.c_str());
        g_eng.set_off(HudOff::left_out);
        return false;
    }
    hlog("check: %zu resource keys used, all in UI.zip's application dictionaries (%d dictionaries, %zu keys)", g_check.used, g_check.dicts,
         g_check.known);
    g_spliced = true;
    return true;
}

void hud_layer_page_written(bool ok, const std::string& page) {
    if (!g_spliced) return;
    if (!ok) {
        hlog("page: A_Hud.xaml couldn't be written; the layer isn't in the game this session");
        g_l13_logged = true;
        g_eng.set_off(HudOff::left_out);
        return;
    }
    g_page_written = true;
    write_marker(false);
    if (FILE* f = _wfopen((fh2mods() / L"platform-hudlayer-A_Hud.xaml").c_str(), L"wb")) {
        std::fwrite(page.data(), 1, page.size(), f);
        std::fclose(f);
    }
    hlog("page: wrote A_Hud.xaml with the layer (%s): %zu properties, %zu bindings (at most %zu), %zu DataTriggers, %zu bytes; a copy is in "
         "fh2-mods\\platform-hudlayer-A_Hud.xaml",
         hud_parts_text(g_parts).c_str(), g_check.distinct, g_check.bindings, kHudMaxBindings, g_check.triggers, page.size());
}

void hud_layer_start() {
    if (!g_active.load() || g_left_out || g_eng.off()) return;
    if (!g_page_written) {
        if (!g_l13_logged) hlog("page: A_Hud.xaml couldn't be written; the layer isn't in the game this session");
        g_l13_logged = true;
        g_eng.set_off(HudOff::left_out);
        return;
    }
    std::thread(start_thread).detach();
    device_sweep_start();
}

uint8_t* hud_layer_find(const wchar_t* name) {
    if (!g_active.load() || !g_spliced || g_eng.off() || !g_code_ok.load(std::memory_order_acquire) || !name) return nullptr;
    const auto it = g_names.find(name + 11);   // after "FH2Mod_Hud_"
    if (it == g_names.end()) return nullptr;
    const int i      = it->second;
    const HudProp& p = g_eng.props()[static_cast<std::size_t>(i)];
    uint8_t* d       = nullptr;
    bool first_shown = false;
    {
        std::scoped_lock lk{g_find_mu};
        d                    = g_descs[i].load();
        const int kind       = p.type == 'b' ? 1 : p.type == 'i' ? 0 : p.type == 'f' ? 2 : 3;
        if (!d && !g_kind_refused[kind]) {
            d = settings_hud_descriptor(p.name.c_str(), reinterpret_cast<void*>(&hud_get), kind);
            if (!d) {
                g_kind_refused[kind] = true;
                hlog("properties: the %s descriptors couldn't be made (the game's code for them doesn't match); the HUD layer is off",
                     hud_type_word(p.type));
                g_eng.set_off(HudOff::descriptors);
                return nullptr;
            }
            *reinterpret_cast<uint32_t*>(d + 0x7F8) = kDescMagic;
            *reinterpret_cast<uint32_t*>(d + 0x800) = static_cast<uint32_t>(i);
            g_descs[i].store(d);
            if (++g_desc_count == g_nprops) hlog("properties: %d descriptors made (4 KB each)", g_desc_count.load());
        }
        first_shown = d && i == g_shown_ix && !g_page_read.exchange(true);
    }
    if (first_shown) write_marker(true);   // the page is being built with the layer
    return d;
}

const char* hud_layer_crash_text() {
    if (!g_active.load()) return "";
    const int ix = g_crash_ix.load();
    return ix < 0 ? "page not built yet" : g_crash[ix];
}

std::string hud_layer_session_text() {
    if (!g_active.load()) return "";
    std::string mods;
    for (int i = 0; i < g_eng.layer_count(); ++i) mods += (i ? ", " : "") + g_eng.layer_name(i);
    return "preview 3: " + mods;
}

void hud_layer_sweep_check() {
    if (!g_active.load() || !g_ready.load() || g_pump_late_logged.load()) return;
    const double last = g_last_pump.load();
    if (g_on_screen.load() && last > 0 && now_ms() - last > 5000 && !g_pump_late_logged.exchange(true))
        hlog("the pump hasn't run for 5 s while the HUD page is on screen");
}

} // namespace fh2platform
