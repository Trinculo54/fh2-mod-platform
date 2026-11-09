// Pop-ups and tips on the game's own HUD notification (the UI templates' renderer G, platform 2.9.0): the glue
// between the queue (popup_queue.cpp, where every rule is) and the game. Achievements' pop-ups are the proven
// pattern this follows, with the static research of S2 and S5 (UI-TEMPLATES-290 1.3 to 1.9 and 2.1):
//  - The tick: an instruction hook on the AchievementsModule's update (0x1403FCD84, about 60 a second on the game's
//    thread), chained with Achievements, CommunityGarage and the HUD layer. It runs the queue, reads the game's
//    Notifications option, and makes the display strings and calls Show there, as Achievements does.
//  - The component: HUDMinorNotificationComponent's constructor and destructor (instruction hooks at 0x14073E0B1 and
//    0x14073F046, rbx = it, chained) keep the list of live ones; Show goes to the "RoadDiscovery" one, else the
//    oldest. Show (0x140755330) and MakeDisplayString (0x140323310) are called, never patched: Achievements 1.0.x
//    checks their first 15 bytes in memory, FH2MP 1.2.0 MakeDisplayString's 16 (rule 1.9).
//  - The watch: hook_call on the three calls of the minor-notification gate (0x140754920) that S2 found: in Show
//    (0x14075542C), in its sibling (0x1407555A5, the game's road discoveries, photo collection and rewards) and in
//    the component's reflected Show (0x140750FB1). Each detour compares the component's +0x198 before and after:
//    the gate stores a new object there only when it lets the pop-up through. So the platform sees every Show the
//    game shows, whoever made it, and knows whether its own was drawn. Without the watch, pop-ups are 5.5 s apart.
//  - The Notifications option (Options/MinorNotifications in the player's profile, S2): read on the tick's thread
//    by the gate's own four calls, once a second and again just before each Show, published in an atomic.
//  - Page tracking: the free-roam HUD's vtable slots +0x488 (shown), +0x4F8 (hidden) and 0 (the deleting
//    destructor), chained with the HUD layer's and the notice's. The game's HUD suspend is read, never set: the
//    HUD modules' prioritiser byte +0xA8, found as the HUD layer finds it.
//  - Text: titles in capitals in the game's language (text.cpp), cut at 28 and 45 characters, and folded to ASCII
//    for the narrow MakeDisplayString (its MultiByteToWideChar uses the Windows code page). The wide path (design
//    1.7: the same allocation, the wide constructor 0x141239840 and the AddRef) is behind WideText=1 in
//    fh2-mods\platform-popups.ini until it has been seen working in game (T10).
// Every game address is checked against the exe on disk before use; a group that doesn't match turns off only what
// needs it, with a log line. Game calls go through one SEH-guarded helper that holds no object with a destructor.
// The hooks go in only in a session where a mod declared pop-ups or tips in fh2mod_init (ui_uses), as soon as the
// game's code is ready: the notification component is made, and the HUD page first shown, long before a mod may send
// its first pop-up (a hotkey's reply, a fault), and only these hooks see either, so they can't wait for the first
// call. A session with no such mod leaves the game's notification code alone (its sites are shared with
// Achievements' and CommunityGarage's hooks), and an undeclared show_toast or show_tip returns 0 with a log line.
// They're read-only and chained, and S5 found every released mod's in-memory checks clear of them.
#include "popups.hpp"

#include "hud_layer.hpp"
#include "lang.hpp"
#include "platform.hpp"
#include "seen.hpp"
#include "text.hpp"
#include "ui_sounds.hpp"
#include "ui_player.hpp"
#include "ui_templates.hpp"
#include "updates.hpp"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace fh2platform {
namespace {

namespace fs = std::filesystem;

// ---- game addresses (game+RVA; the full address in each comment) ------------------------------------------------
constexpr uintptr_t kImageBase = 0x140000000;
// the tick
constexpr uintptr_t RVA_TICK_FN = 0x3FCD60;           // 0x1403FCD60 AchievementsModule +0x20
constexpr uintptr_t RVA_TICK    = 0x3FCD84;           // 0x1403FCD84 its mov eax,[rcx+0xA8]
// the notification component
constexpr uintptr_t RVA_NOTIF_CTOR_AT = 0x73E0B1;     // 0x14073E0B1 in its constructor: rbx = it
constexpr uintptr_t RVA_NOTIF_DTOR_AT = 0x73F046;     // 0x14073F046 in its destructor: rbx = it
constexpr std::size_t kTemplateName   = 0x128 + 0x18; // its TemplateName, an MSVC std::string
constexpr std::size_t kNotifData      = 0x198;        // the gate's object: a new one only when a pop-up is let through
constexpr uintptr_t RVA_SHOW          = 0x755330;     // 0x140755330 Show(component, main, sub, icon)
constexpr uintptr_t RVA_MAKE_STRING   = 0x323310;     // 0x140323310 MakeDisplayString(out, const char*, length)
// the watch: the minor-notification gate and its three callers
constexpr uintptr_t RVA_GATE                = 0x754920;   // 0x140754920
constexpr uintptr_t kGateCalls[3]           = {0x75542C, 0x7555A5, 0x750FB1};   // in Show, its sibling, the reflected Show
const char* const kGateCallNames[3]         = {"Show", "Show's sibling (road discoveries, photos, rewards)", "the component's reflected Show"};
// the Notifications option (S2)
constexpr uintptr_t RVA_FORZA        = 0x34ABA60;     // 0x1434ABA60 CForza
constexpr uintptr_t RVA_PLAYERS      = 0x34B3E18;     // 0x1434B3E18 CPlayerManager
constexpr uintptr_t RVA_PROFILE      = 0xB2BC80;      // 0x140B2BC80 the player's CForzaProfile, or 0
constexpr uintptr_t RVA_LOOKUP       = 0x2E11F0;      // 0x1402E11F0 lookup(profile, CVariant* out, path)
constexpr uintptr_t RVA_VARIANT_DTOR = 0x107D880;     // 0x14107D880 ~CVariant
constexpr uintptr_t RVA_OPTION_PATH  = 0x1E06AF8;     // 0x141E06AF8 "Options/MinorNotifications"
// the free-roam HUD page (Hud, vtable 0x142DEF9C8)
constexpr uintptr_t RVA_HUD_VT          = 0x2DEF9C8;  // slot 0 = deleting destructor
constexpr uintptr_t RVA_HUD_DTOR        = 0xD244C0;   // 0x140D244C0
constexpr uintptr_t RVA_HUD_SHOWN_SLOT  = 0x2DEFE50;  // 0x142DEFE50 vtable +0x488
constexpr uintptr_t RVA_HUD_SHOWN       = 0xD38DD0;   // 0x140D38DD0
constexpr uintptr_t RVA_HUD_HIDDEN_SLOT = 0x2DEFEC0;  // 0x142DEFEC0 vtable +0x4F8
constexpr uintptr_t RVA_HUD_HIDDEN      = 0xD41540;   // 0x140D41540
// the game's HUD suspend, read only (the HUD layer's walk)
constexpr uintptr_t RVA_WM_PTR          = 0x34C0F40;  // 0x1434C0F40 the WorldManager pointer
constexpr uintptr_t RVA_WM_VT           = 0x2CD1F20;  // .?AVWorldManager@EntityModel@@
constexpr uintptr_t RVA_HUDMODULE_VT    = 0x2CDC7E0;  // .?AVHUDModule@EntityModel@@
constexpr uintptr_t RVA_HUDMODULE_AR_VT = 0x2CDC890;  // .?AVHUDModuleAR@EntityModel@@
constexpr uintptr_t RVA_PRIO_VT         = 0x2CDC5E8;  // .?AVHUDStatusPrioritiserModule@EntityModel@@
constexpr std::size_t kPrioSuspended    = 0xA8;
// the wide display string (design 1.7, behind WideText=1)
constexpr uintptr_t RVA_TLS_INDEX    = 0x354EBD8;     // 0x14354EBD8 the per-thread slot MakeDisplayString's heap comes from
constexpr uintptr_t RVA_STRING_HEAP  = 0x125F380;     // 0x14125F380
constexpr uintptr_t RVA_STRING_ALLOC = 0x125D850;     // 0x14125D850 alloc(heap, size, tag, 0)
constexpr uintptr_t RVA_STRING_WCTOR = 0x1239840;     // 0x141239840 the wide constructor, in place

// ---- small helpers ----------------------------------------------------------------------------------------------

FH2Mod make_owner() {
    FH2Mod m;
    m.name          = "Pop-ups";
    m.wname         = L"Pop-ups";
    m.check_updates = false;
    return m;
}
FH2Mod g_owner = make_owner();   // owns the hooks and the platform's own lines; never registered
std::atomic<const FH2ModApi*> g_api{nullptr};
std::atomic<uintptr_t> g_base{0};
uintptr_t G(uintptr_t rva) { return g_base.load(std::memory_order_relaxed) + rva; }
void* GP(uintptr_t rva) { return reinterpret_cast<void*>(G(rva)); }

LARGE_INTEGER qpf() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f;
}
const LARGE_INTEGER g_qpf = qpf();
double now_ms() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(g_qpf.QuadPart);
}

__attribute__((format(printf, 1, 2))) void plog_f(const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    log_line(g_owner.name, b);
}

template <class T> bool rd(uintptr_t a, T& v) { return a >= 0x10000 && safe_copy(&v, reinterpret_cast<const void*>(a), sizeof v); }
uintptr_t rdp(uintptr_t a) {
    uintptr_t v = 0;
    return rd(a, v) ? v : 0;
}
uintptr_t vtable_of(const void* obj) { return obj ? rdp(reinterpret_cast<uintptr_t>(obj)) : 0; }

// An MSVC std::string in game memory (buffer or pointer at +0, size at +0x10, capacity at +0x18), or "".
std::string game_string(uintptr_t at) {
    uint64_t size = 0, cap = 0;
    if (!rd(at + 0x10, size) || !rd(at + 0x18, cap) || size > 256 || cap < size) return "";
    const uintptr_t p = cap >= 16 ? rdp(at) : at;
    std::string s(static_cast<std::size_t>(size), '\0');
    if (size && !safe_copy(s.data(), reinterpret_cast<const void*>(p), s.size())) return "";
    return s;
}

// One SEH frame for every game call: the call is a captureless thunk over a plain context.
bool guarded(void (*thunk)(void*), void* ctx, DWORD* code) {
    __try {
        thunk(ctx);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- code checks against the exe on disk ------------------------------------------------------------------------

struct Entry {
    char kind;        // 'B' bytes, 'Q' a qword (a vtable slot), 'T' an E8 rel32 to a target, 'S' a C string
    uintptr_t rva;
    const char* hex;  // B: the bytes; S: the text (its NUL is compared too)
    uint64_t value;   // Q: the qword; T: the full target address
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
        const char b[3] = {s[0], s[1], 0};
        out[n++]        = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
        s += 2;
    }
    return n;
}

// Every entry as the exe on disk has it, logged either way; on a mismatch, the first address and what goes.
bool check_group(const char* group, const char* what_goes, const std::vector<Entry>& es) {
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
            const auto rel = static_cast<int32_t>(static_cast<int64_t>(e.value) - static_cast<int64_t>(kImageBase + e.rva + 5));
            want[0]        = 0xE8;
            std::memcpy(want + 1, &rel, 4);
            n = 5;
        }
        if (!n || !disk_bytes_equal(GP(e.rva), want, n)) {
            plog_f("code: %s: game+0x%llX doesn't match the exe on disk; %s", group, static_cast<unsigned long long>(e.rva), what_goes);
            return false;
        }
    }
    plog_f("code: %s: as expected", group);
    return true;
}

// ---- the session's state ----------------------------------------------------------------------------------------

std::once_flag g_start_once;
std::atomic<bool> g_started{false};      // a mod declared pop-ups or tips in fh2mod_init, so the hooks are on their way
std::atomic<bool> g_prepared{false};     // every mod's fh2mod_init has returned, so the declarations are known
std::atomic<bool> g_track_wanted{false}; // the page tracking hooks are asked for (F1)
std::atomic<bool> g_ready{false};        // the tick, the component hooks and their code checks are in
std::atomic<bool> g_watch{false};        // all three gate calls are watched
std::atomic<bool> g_tracking{false};     // the page is tracked
std::atomic<bool> g_suspend_ok{false};   // the HUD suspend can be read
std::atomic<bool> g_option_ok{false};    // the Notifications option can be read
std::atomic<bool> g_wide{false};         // WideText=1, and the wide constructor's code checked
std::atomic<bool> g_tips_poll{false};    // the controller poll was started for tips
std::atomic<DWORD> g_tick_thread{0};
std::atomic<int> g_option{-1};           // 1 on, 0 off, -1 not known
std::atomic<int> g_force_device{-1};     // "fh2mod Platform ui keyboard | pad": 1 keys, 0 the pad, -1 as last used

// The device used last (1 the keyboard or mouse), or the one a developer forced.
bool keyboard_last() {
    const int forced = g_force_device.load();
    return forced >= 0 ? forced == 1 : device_auto_kbd() == 1;
}
double g_option_at = -1e18;              // the tick only
bool g_option_unread_logged = false;     // the tick only
int g_option_logged         = -2;        // the tick only: the value last logged

PopupQueue& queue();

PopupMod mod_ref(FH2Mod* m) {
    PopupMod r;
    r.id   = m;
    r.name = m->name;
    r.dll  = narrow(m->dll);
    r.uses = ui_uses_of(m);
    return r;
}

// ---- the components ---------------------------------------------------------------------------------------------

std::mutex g_comp_mu;
std::vector<void*> g_comps;   // live HUDMinorNotificationComponents, oldest first

void on_notif_ctor(FH2Context* c, void*) {
    std::scoped_lock lk{g_comp_mu};
    g_comps.push_back(reinterpret_cast<void*>(c->rbx));
}
void on_notif_dtor(FH2Context* c, void*) {
    std::scoped_lock lk{g_comp_mu};
    g_comps.erase(std::remove(g_comps.begin(), g_comps.end(), reinterpret_cast<void*>(c->rbx)), g_comps.end());
}

// The HUD's "road discovered" notification (an icon and two lines, made first and living as long as the HUD), else
// the oldest, as Achievements picks it.
void* pick_component() {
    std::scoped_lock lk{g_comp_mu};
    for (void* c : g_comps)
        if (game_string(reinterpret_cast<uintptr_t>(c) + kTemplateName) == "RoadDiscovery") return c;
    return g_comps.empty() ? nullptr : g_comps.front();
}
bool have_component() {
    std::scoped_lock lk{g_comp_mu};
    return !g_comps.empty();
}

// ---- the watch --------------------------------------------------------------------------------------------------

using GateFn = uint64_t (*)(void*, void*, void*, void*, void*);
GateFn o_gate[3]               = {};
std::atomic<void*> g_our_comp{nullptr};   // the component the platform's own Show is on, while it runs
std::atomic<bool> g_our_drawn{false};

void gate_let_through(void* comp) {
    if (comp && comp == g_our_comp.load() && GetCurrentThreadId() == g_tick_thread.load()) g_our_drawn = true;
    queue().show_seen(now_ms());
}

// The gate stores a new object at the component's +0x198 only when it lets a pop-up through (S2 1.5).
template <int N> uint64_t d_gate(void* comp, void* a, void* b, void* c, void* d) {
    uint64_t before = 0, after = 0;
    const bool read = comp && rd(reinterpret_cast<uintptr_t>(comp) + kNotifData, before);
    const uint64_t r = o_gate[N](comp, a, b, c, d);
    if (read && rd(reinterpret_cast<uintptr_t>(comp) + kNotifData, after) && after != before) gate_let_through(comp);
    return r;
}
void* const kGateDetours[3] = {reinterpret_cast<void*>(&d_gate<0>), reinterpret_cast<void*>(&d_gate<1>), reinterpret_cast<void*>(&d_gate<2>)};

// ---- the page and the suspend -----------------------------------------------------------------------------------

using HudFn = uint64_t (*)(void*, void*, void*, void*);
HudFn o_shown = nullptr, o_hidden = nullptr, o_dtor = nullptr;
std::atomic<void*> g_page{nullptr};
std::atomic<bool> g_on_screen{false};

bool page_is_hud(void* page) { return page && vtable_of(page) == G(RVA_HUD_VT); }

uint64_t d_shown(void* page, void* b, void* c, void* d) {
    const uint64_t r = o_shown(page, b, c, d);
    if (page_is_hud(page)) {
        g_page      = page;
        g_on_screen = true;
    }
    return r;
}
uint64_t d_hidden(void* page, void* b, void* c, void* d) {
    const uint64_t r = o_hidden(page, b, c, d);
    if (page && page == g_page.load()) g_on_screen = false;
    return r;
}
uint64_t d_dtor(void* page, void* flags, void* c, void* d) {
    void* tracked = page;
    if (page && g_page.compare_exchange_strong(tracked, nullptr)) g_on_screen = false;   // forgotten before it's freed
    return o_dtor(page, flags, c, d);
}

// The world's HUD modules and their prioritisers, by exact vtable (the tick only), as the HUD layer finds them.
struct Modules {
    uintptr_t wm = 0, world = 0;
    uintptr_t mod[2] = {}, prio[2] = {};
    bool found = false;
} g_mods;
double g_last_walk = -1e18;

uintptr_t prio_of(uintptr_t m) {   // module+0x68, its control block +0x70 with uses > 0
    uintptr_t ctrl = 0, prio = 0;
    int32_t uses   = 0;
    if (!m || !rd(m + 0x70, ctrl) || !ctrl || !rd(ctrl + 8, uses) || uses <= 0 || !rd(m + 0x68, prio) || !prio ||
        vtable_of(reinterpret_cast<void*>(prio)) != G(RVA_PRIO_VT))
        return 0;
    return prio;
}

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
            if (vt && vt == G(RVA_HUDMODULE_VT) && !out.mod[0]) out.mod[0] = obj;
            if (vt && vt == G(RVA_HUDMODULE_AR_VT) && !out.mod[1]) out.mod[1] = obj;
        }
        node = rdp(node);
    }
    for (int k = 0; k < 2; ++k) out.prio[k] = prio_of(out.mod[k]);
    out.found = out.mod[0] || out.mod[1];
    return out.found;
}

// Whether the game has its HUD suspended now: the modules cached, checked each tick, walked again once a second
// while none are known.
bool suspended_now(double now) {
    if (!g_suspend_ok.load()) return false;
    if (g_mods.found) {
        const uintptr_t wm = rdp(G(RVA_WM_PTR)), world = wm ? rdp(wm + 8) : 0;
        bool changed                  = wm != g_mods.wm || world != g_mods.world;
        static const uintptr_t kVt[2] = {RVA_HUDMODULE_VT, RVA_HUDMODULE_AR_VT};
        for (int k = 0; k < 2 && !changed; ++k)
            if (g_mods.mod[k] && (vtable_of(reinterpret_cast<void*>(g_mods.mod[k])) != G(kVt[k]) || prio_of(g_mods.mod[k]) != g_mods.prio[k]))
                changed = true;
        if (changed) {
            g_mods      = Modules{};
            g_last_walk = now;
            return false;
        }
    } else {
        if (now - g_last_walk < 1000) return false;
        g_last_walk = now;
        if (!walk_modules(g_mods)) return false;
    }
    for (int k = 0; k < 2; ++k) {
        uint8_t f = 0;
        if (g_mods.prio[k] && rd(g_mods.prio[k] + kPrioSuspended, f) && f == 1) return true;
    }
    return false;
}

// ---- the Notifications option (S2 1.4) --------------------------------------------------------------------------

struct OptionRead {
    uintptr_t base;
    bool store;
    int value, type;
};
__attribute__((noinline)) void option_thunk(void* p) {
    auto* c     = static_cast<OptionRead*>(p);
    void* store = reinterpret_cast<void* (*)()>(c->base + RVA_PROFILE)();
    c->store    = store != nullptr;
    if (!store) return;
    alignas(16) unsigned char v[0x20] = {};   // a CVariant is 0x18 bytes
    reinterpret_cast<void (*)(void*, void*, const char*)>(c->base + RVA_LOOKUP)(store, v, reinterpret_cast<const char*>(c->base + RVA_OPTION_PATH));
    std::memcpy(&c->value, v + 8, sizeof c->value);
    std::memcpy(&c->type, v + 0x10, sizeof c->type);
    reinterpret_cast<void (*)(void*)>(c->base + RVA_VARIANT_DTOR)(v);
}

// The tick only. The game reads it when a pop-up is sent, so once a second and just before each Show is enough.
void read_option(double now) {
    g_option_at = now;
    if (!g_option_ok.load()) return;
    if (!rdp(G(RVA_FORZA)) || !rdp(G(RVA_PLAYERS))) {   // before start-up or after shutdown
        g_option = -1;
        return;
    }
    OptionRead r{g_base.load(), false, 0, -1};
    DWORD code = 0;
    if (!guarded(option_thunk, &r, &code)) {
        g_option_ok = false;
        g_option    = -1;
        plog_f("reading the game's Notifications setting crashed (exception 0x%08lX), so it isn't read again this session; pop-ups are sent as "
               "if it were on, and the watch tells whether the game drew them",
               static_cast<unsigned long>(code));
        return;
    }
    const int v = !r.store || r.type == -1 ? -1 : r.value > 0 ? 1 : 0;   // the game's own test is "jg"
    g_option    = v;
    if (v == -1 && !g_option_unread_logged) {
        g_option_unread_logged = true;
        plog_f("the game's Notifications setting can't be read yet (no player profile loaded)");
    }
    if (v != -1 && v != g_option_logged) {
        g_option_logged = v;
        plog_f("the game's Notifications setting (Settings > HUD) reads %s", v ? "on" : "off: mod pop-ups and tips don't show while it's off");
    }
}

// ---- Show -------------------------------------------------------------------------------------------------------

struct ShowCall {
    uintptr_t base;
    void* comp;
    bool wide;
    const char* text[3];
    const wchar_t* wtext[2];
};

// The wide copy of MakeDisplayString (design 1.7): its allocation from the per-thread heap, the wide constructor in
// place of the narrow one, and the AddRef, exactly as 0x14032333F-0x1403233AB does them. The tick's thread only.
__attribute__((noinline)) void make_wide(uintptr_t base, void** out, const wchar_t* text) {
    const uint32_t index = *reinterpret_cast<const uint32_t*>(base + RVA_TLS_INDEX);
    auto* const* slots   = reinterpret_cast<uint8_t* const*>(__readgsqword(0x58));
    const uint16_t tag   = *reinterpret_cast<const uint16_t*>(slots[index] + 0x124);
    void* heap           = reinterpret_cast<void* (*)(uint64_t)>(base + RVA_STRING_HEAP)(0x124);
    void* mem            = reinterpret_cast<void* (*)(void*, uint64_t, uint64_t, uint64_t)>(base + RVA_STRING_ALLOC)(heap, 0x18, tag, 0);
    void* obj            = mem ? reinterpret_cast<void* (*)(void*, const wchar_t*, int)>(base + RVA_STRING_WCTOR)(mem, text, -1) : nullptr;
    *out                 = nullptr;
    if (obj) reinterpret_cast<void (*)(void*)>((*reinterpret_cast<void***>(obj))[2])(obj);
    *out = obj;
}

// The three display strings, then Show, which takes them by value and releases them itself (as Achievements does).
__attribute__((noinline)) void show_thunk(void* p) {
    auto* c     = static_cast<ShowCall*>(p);
    auto make   = reinterpret_cast<void** (*)(void**, const char*, int)>(c->base + RVA_MAKE_STRING);
    void* s[3]  = {};
    for (int i = 0; i < 2; ++i) {
        if (c->wide) make_wide(c->base, &s[i], c->wtext[i]);
        else make(&s[i], c->text[i], -1);
    }
    make(&s[2], c->text[2], -1);   // the icon's name is always ASCII
    reinterpret_cast<void (*)(void*, void**, void**, void**)>(c->base + RVA_SHOW)(c->comp, &s[0], &s[1], &s[2]);
}

// A pop-up's sound (FH2_SOUND_ 1 to 11; NOTIFY is never played with one: the game chimes), through play_ui_sound's
// player and its checks (ui_sounds.cpp), so it doesn't need the MODS tab's hooks.
void play_sound(uint32_t sound) {
    uint32_t code = 0;
    if (!sound || ui_sound_play_direct(sound, &code) || !code) return;
    plog_f("a pop-up's sound crashed in the game's code (exception 0x%08X)", code);
}

void show_now(const PopupShow& s, double now) {
    if (g_option_ok.load()) {
        read_option(now);
        if (g_option.load() == 0) {
            queue().outcome(s.id, PopupOutcome::refused, now);
            return;
        }
    }
    void* comp = pick_component();
    if (!comp) {
        queue().outcome(s.id, PopupOutcome::failed, now);
        return;
    }
    const bool wide          = g_wide.load();
    const std::wstring wt    = wide ? widen(s.title) : std::wstring(), wl = wide ? widen(s.line) : std::wstring();
    ShowCall c{g_base.load(), comp, wide, {s.title.c_str(), s.line.c_str(), s.icon.c_str()}, {wt.c_str(), wl.c_str()}};
    g_our_drawn = false;
    g_our_comp  = comp;
    DWORD code  = 0;
    const bool ok = guarded(show_thunk, &c, &code);
    g_our_comp    = nullptr;
    if (!ok) {
        plog_f("showing %s's pop-up crashed in the game's code (exception 0x%08lX, %s text); no more pop-ups this session", s.mod.c_str(),
               static_cast<unsigned long>(code), wide ? "wide" : "narrow");
        queue().outcome(s.id, PopupOutcome::failed, now);
        queue().set_available(0, "showing a pop-up crashed in the game's code earlier this session");
        return;
    }
    const PopupOutcome o = !g_watch.load() ? PopupOutcome::unknown : g_our_drawn.load() ? PopupOutcome::shown : PopupOutcome::refused;
    queue().outcome(s.id, o, now);
    if (o != PopupOutcome::refused) play_sound(s.sound);
}

// ---- the tick ---------------------------------------------------------------------------------------------------

void on_tick(FH2Context*, void*) {
    if (!g_ready.load(std::memory_order_acquire)) return;
    const double now = now_ms();
    DWORD none       = 0;
    g_tick_thread.compare_exchange_strong(none, GetCurrentThreadId());
    if (now - g_option_at >= 1000) read_option(now);
    PopupTick in;
    in.now       = now;
    in.on_screen = !g_tracking.load() || g_on_screen.load();   // untracked: as if always on screen
    in.suspended = suspended_now(now);
    in.option    = g_option.load();
    in.keyboard  = keyboard_last();
    in.ready     = have_component();
    in.wide      = g_wide.load();
    const PopupShow s = queue().tick(in);
    if (s.id) show_now(s, now);
}

// ---- start-up ---------------------------------------------------------------------------------------------------

void off(const std::string& why) {
    plog_f("pop-ups and tips are off this session: %s", why.c_str());
    queue().set_available(0, why);
}

// Hooks all or none: true with every one on, else every one made is off again (and logged).
bool hook_all(FH2Hook** made, int n, const char* const* names) {
    const FH2ModApi* api = g_api.load();
    bool all             = true;
    for (int i = 0; i < n; ++i) all = all && made[i];
    for (int i = 0; i < n && all; ++i) all = api->enable_hooks(&g_owner, made[i]) != 0;
    if (all) return true;
    for (int i = 0; i < n; ++i) {
        if (made[i]) api->disable_hooks(&g_owner, made[i]);
        else plog_f("hooks: %s refused (see the line above)", names[i]);
    }
    return false;
}

void start_thread() {
    const FH2ModApi* api = g_api.load();
    void* base           = api->wait_for_code(0xFFFFFFFFu);
    if (!base) return off("the game's code never became ready (or Azimuth started another game)");
    g_base = reinterpret_cast<uintptr_t>(base);
    if (!disk_image_readable()) return off("the game exe couldn't be read from the game folder, so the game's notification code can't be checked");
    const bool required = check_group("pop-ups: the tick", "pop-ups and tips are off",
                                      {B(RVA_TICK_FN, "40 57 48 83 EC 60 48 C7 44 24 20 FE FF FF FF"), B(RVA_TICK, "8B 81 A8 00 00 00")}) &&
                          check_group("pop-ups: the notification component", "pop-ups and tips are off",
                                      {
                                          // the constructor: rbx = the component, +0x198 cleared just after the site
                                          B(0x73E0A5, "48 8D 8B 60 01 00 00 E8 9F 0F CA FF 48 8D 8B A0 01 00 00 48 C7 83 98 01 00 00 00 00 00 00"),
                                          // the destructor: rbx = the component
                                          B(0x73F032, "48 8B D9 48 8D 05 34 8B 59 02 48 89 01 48 8D 05 62 8B 59 02 48 89 41 28 48 81 C1 A0 01 00 00"),
                                      }) &&
                          check_group("pop-ups: Show and MakeDisplayString", "pop-ups and tips are off",
                                      {
                                          B(RVA_SHOW, "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10"),
                                          B(RVA_MAKE_STRING, "40 57 48 83 EC 30 48 C7 44 24 28 FE FF FF FF"),
                                      });
    if (!required) return off("the game's notification code isn't as expected (see the line above)");
    const bool watch_code = check_group("pop-ups: the watch", "pop-ups are spaced 5.5 s apart and may meet the game's own",
                                        {
                                            T(kGateCalls[0], kImageBase + RVA_GATE),
                                            T(kGateCalls[1], kImageBase + RVA_GATE),
                                            T(kGateCalls[2], kImageBase + RVA_GATE),
                                            B(0x7549C3, "48 C7 44 24 68 00 00 00 00 48 8D 54 24 48 48 8D 8E A0 01 00 00 E8 43 6C C9 FF"),
                                            B(0x754A11, "48 8B 03 48 89 86 98 01 00 00"),   // the new object stored at +0x198
                                        });
    g_option_ok = check_group("pop-ups: the Notifications setting", "it isn't read: pop-ups are sent as if it were on",
                              {
                                  B(RVA_PROFILE, "48 83 EC 28 48 8B 0D D5 FD 97 02 E8 E0 23 6F FF 48 8B 08 48 83 C4 28 E9 E4 21 7B FF"),
                                  B(0x21E070, "48 8D 81 A8 01 00 00 C3"),   // CForza+0x1A8
                                  B(0x327270, "48 8B 05 A1 CB 18 03 C3"),   // [game+0x34B3E18]
                                  B(RVA_LOOKUP, "48 8B C4 48 89 50 10 57 48 81 EC A0 00 00 00 48"),
                                  B(0x2E1230, "80 7F 38 00 74 56"),         // "loaded": the type reads -1 before
                                  B(RVA_VARIANT_DTOR, "48 8D 05 39 D3 DF 01 83 CA FF 48 89 01 E9 3E 95 00 00"),
                                  // the gate reads it this way: profile, lookup, the int at +8, then "test ebx,ebx; jg show"
                                  B(0x75495C, "E8 1F 73 3D 00 48 85 C0 74 29 4C 8D 05 8B 21 6B 01 48 8D 54 24 70 48 8B C8 E8 76 C8 B8 FF 90 48 8B C8 "
                                              "E8 2D FE 1C 00 8B D8 48 8D 4C 24 70 E8 F1 8E 92 00 E8 4C E6 CB 00 48 8B 10 48 8B C8 FF 92 80 01 00 00 "
                                              "4C 8B A4 24 F0 00 00 00 85 DB 7F 09 83 F8 53 0F 85 9E 01 00 00"),
                                  S(RVA_OPTION_PATH, "Options/MinorNotifications"),
                              });
    const bool page_code = check_group("pop-ups: the HUD page", "pop-ups are sent as if the driving view were always up",
                                       {
                                           Q(RVA_HUD_VT, kImageBase + RVA_HUD_DTOR),
                                           B(RVA_HUD_DTOR, "48 89 4C 24 08 57 48 83 EC 30 48 C7 44 24 20 FE"),
                                           Q(RVA_HUD_SHOWN_SLOT, kImageBase + RVA_HUD_SHOWN),
                                           B(RVA_HUD_SHOWN, "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 E0"),
                                           Q(RVA_HUD_HIDDEN_SLOT, kImageBase + RVA_HUD_HIDDEN),
                                           B(RVA_HUD_HIDDEN, "48 8B C4 55 48 8D 68 A1 48 81 EC F0 00 00 00 48"),
                                       });
    g_suspend_ok = check_group("pop-ups: the game's HUD suspend", "it isn't read: pop-ups may be sent while the game's HUD is suspended, and lost",
                               {
                                   B(0x921215, "48 8B 0D 24 FD B9 02"),   // reads the WorldManager pointer game+0x34C0F40
                                   B(0x91A476, "48 81 C1 F8 00 00 00"),   // the world's module list at +0xF8
                                   B(0x78B161, "C6 81 A8 00 00 00 01"),   // HUDModule's suspend sets the prioritiser's +0xA8
                                   B(0x78B211, "C6 81 A8 00 00 00 01"),   // HUDModuleAR's
                                   B(0x785CE7, "C6 81 A8 00 00 00 00"),   // the resume clears it
                               });
    if (g_wide.load())
        g_wide = check_group("pop-ups: wide text", "pop-up text is folded to ASCII after all",
                             {
                                 B(0x32333F, "44 8B 0D 92 B8 22 03 65 48 8B 04 25 58 00 00 00 B9 24 01 00 00 4A 8B 04 C8 0F B7 1C 01 E8 1F C0 F3 00 45 33 "
                                             "C9 44 0F B7 C3 41 8D 51 18 48 8B C8 E8 DC A4 F3 00 48 89 44 24 40 48 85 C0 74 13 44 8B C6 48 8B D5 48 8B C8 "
                                             "E8 84 64 F1 00 48 8B D8 EB 02 33 DB 48 C7 07 00 00 00 00 48 85 DB 74 09 48 8B 03 48 8B CB FF 50 10"),
                                 B(RVA_STRING_WCTOR, "40 53 48 83 EC 20 48 8D 05 53 76 C7 01 48 8B D9 48 89 01 33 C0 48 89 41 08 48 89 41 10 E8 4E CF 00 00"),
                                 B(RVA_STRING_HEAP, "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF"),
                                 B(RVA_STRING_ALLOC, "4C 8B DC 57 41 56 41 57 48 81 EC 60 02 00 00"),
                             });
    // the tick and the component hooks, all or none
    FH2Hook* req[3] = {
        api->hook_instruction(&g_owner, "AchievementsModule update (the pop-ups' tick)", GP(RVA_TICK), &on_tick, nullptr),
        api->hook_instruction(&g_owner, "HUDMinorNotificationComponent constructor (pop-ups)", GP(RVA_NOTIF_CTOR_AT), &on_notif_ctor, nullptr),
        api->hook_instruction(&g_owner, "HUDMinorNotificationComponent destructor (pop-ups)", GP(RVA_NOTIF_DTOR_AT), &on_notif_dtor, nullptr),
    };
    const char* const req_names[3] = {"the tick", "the component's constructor", "the component's destructor"};
    if (!hook_all(req, 3, req_names)) return off("the game's notification couldn't be hooked (see the lines above)");
    // the watch: all three calls or none, since one missed call is a Show not seen
    if (watch_code) {
        FH2Hook* w[3];
        const char* names[3];
        for (int i = 0; i < 3; ++i) {
            names[i] = kGateCallNames[i];
            w[i]     = api->hook_call(&g_owner, (std::string("the minor-notification gate's call in ") + kGateCallNames[i] + " (pop-ups' watch)").c_str(),
                                      GP(kGateCalls[i]), kGateDetours[i], reinterpret_cast<void**>(&o_gate[i]));
        }
        g_watch = hook_all(w, 3, names);
        if (!g_watch.load()) plog_f("the watch on the game's own pop-ups isn't in: pop-ups are spaced 5.5 s apart and may meet the game's own");
    }
    // page tracking: all three or none
    if (page_code) {
        FH2Hook* p[3] = {
            api->hook_pointer(&g_owner, "freeroam HUD: shown (pop-ups)", static_cast<void**>(GP(RVA_HUD_SHOWN_SLOT)), reinterpret_cast<void*>(&d_shown),
                              reinterpret_cast<void**>(&o_shown)),
            api->hook_pointer(&g_owner, "freeroam HUD: hidden (pop-ups)", static_cast<void**>(GP(RVA_HUD_HIDDEN_SLOT)), reinterpret_cast<void*>(&d_hidden),
                              reinterpret_cast<void**>(&o_hidden)),
            api->hook_pointer(&g_owner, "freeroam HUD: deleting destructor (pop-ups)", static_cast<void**>(GP(RVA_HUD_VT)), reinterpret_cast<void*>(&d_dtor),
                              reinterpret_cast<void**>(&o_dtor)),
        };
        const char* const names[3] = {"the HUD page's shown", "the HUD page's hidden", "the HUD page's destructor"};
        g_tracking                 = hook_all(p, 3, names);
    }
    if (!g_tracking.load()) plog_f("the HUD page isn't tracked: pop-ups are sent as if the driving view were always up (under a menu the game may show them later)");
    queue().set_watch(g_watch.load());
    queue().set_available(1);
    g_ready.store(true, std::memory_order_release);
    plog_f("pop-ups and tips are ready: the watch on the game's own %s, the HUD page %s, the Notifications setting %s, the HUD suspend %s, text %s",
           g_watch.load() ? "in" : "not in", g_tracking.load() ? "tracked" : "not tracked", g_option_ok.load() ? "read" : "not read",
           g_suspend_ok.load() ? "read" : "not read", g_wide.load() ? "wide (WideText=1)" : "folded to ASCII");
}

void start() {
    if (!g_api.load()) return;   // popups_prepare starts it
    std::call_once(g_start_once, [] {
        g_started      = true;
        g_track_wanted = true;
        std::thread(start_thread).detach();
    });
}

// A mod sent pop-ups or tips: the hooks are in already (or on their way), so only tips' controller poll is left.
void want(bool tips) {
    if (tips && !g_tips_poll.exchange(true)) device_sweep_start();
    start();
}

// The key's name on the player's keyboard layout ("Entf" on a German one), as the hotkey rows name keys.
std::string layout_key_name(uint32_t vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {   // extended keys, so the name is the key's own, not the numpad's
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU: sc |= 0x100; break;
        default: break;
    }
    wchar_t name[64] = {};
    return sc && GetKeyNameTextW(static_cast<LONG>(sc << 16), name, 64) > 0 ? narrow(name) : std::string();
}

PopupQueue& queue() {
    static PopupQueue q{[] {
        PopupEnv e;
        e.log        = [](const std::string& who, const std::string& line) { log_line(who, line); };
        e.upper      = [](const std::string& s) { return upper_game(s); };
        e.tr         = [](const char* english) { return tr(english); };
        e.seen_get   = [](const std::string& who, const std::string& key) { return seen_get(who, key); };
        e.seen_set   = [](const std::string& who, const std::string& key, const std::string& v) { return seen_set(who, key, v); };
        e.seen_clear = [](const std::string& who, const std::string& key) { return seen_clear(who, key); };
        e.seen_clear_tips = [] {
            return seen_clear_if([](const std::string& who, const std::string& key, const std::string&) { return who != kSeenPlatform && key.rfind("tip.", 0) == 0; });
        };
        e.hotkey = [](const void* mod, const std::string& key, uint32_t* vk, uint32_t* mods, uint32_t* chord) {
            return hotkey_binding(static_cast<const FH2Mod*>(mod), key, vk, mods, chord);
        };
        e.key_name   = layout_key_name;
        e.clock_text = [] {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char b[8];
            std::snprintf(b, sizeof b, "%02d:%02d", t.wHour, t.wMinute);
            return std::string(b);
        };
        return e;
    }()};
    return q;
}

} // namespace

// ---- the API's members ------------------------------------------------------------------------------------------

uint32_t popups_status(FH2Mod* m) {
    uint32_t b = queue().status(m, keyboard_last()) & ~static_cast<uint32_t>(FH2_UI_ON_SCREEN);
    if (g_tracking.load() && g_on_screen.load()) b |= FH2_UI_ON_SCREEN;
    return b;
}

// A pop-up or tip from a session where no mod declared them: said once a mod.
static void undeclared_note(FH2Mod* m, const char* what) {
    static std::mutex mu;
    static std::set<FH2Mod*> told;
    {
        std::scoped_lock lk{mu};
        if (!told.insert(m).second) return;
    }
    log_line(m->name, std::string(what) + ": no mod declared pop-ups or tips in fh2mod_init (ui_uses with FH2_UI_USES_TOASTS or " +
                          "FH2_UI_USES_TIPS), so the platform left the game's notification alone this session and this doesn't show");
}

int popups_show_toast(FH2Mod* m, const FH2Toast* t) {
    if (!m) return 0;
    if (stopped_online(m)) {
        log_line(m->name, "show_toast: this mod was stopped for online play, so its pop-ups don't show");
        return 0;
    }
    if (g_prepared.load() && !g_started.load()) {   // no mod declared pop-ups, so the game's notification was left alone
        undeclared_note(m, "show_toast");
        return 0;
    }
    return queue().show_toast(mod_ref(m), t, now_ms());
}

void popups_clear_toast(FH2Mod* m, const char* key) {
    if (m) queue().clear_toast(mod_ref(m), key);
}

int popups_show_tip(FH2Mod* m, const FH2Tip* t) {
    if (!m) return 0;
    if (stopped_online(m)) {
        log_line(m->name, "show_tip: this mod was stopped for online play, so its tips don't show");
        return 0;
    }
    if (g_prepared.load() && !g_started.load()) {
        undeclared_note(m, "show_tip");
        return 0;
    }
    want(true);   // tips' controller poll, if no mod asked for tips at start
    return queue().show_tip(mod_ref(m), t, now_ms());
}

int popups_tip_seen(FH2Mod* m, const char* key) { return m ? queue().tip_seen(mod_ref(m), key) : 0; }

void popups_set_tip_seen(FH2Mod* m, const char* key, int seen) {
    if (m) queue().set_tip_seen(mod_ref(m), key, seen);
}

// ---- the platform -----------------------------------------------------------------------------------------------

void popups_prepare(const FH2ModApi* api) {
    const fs::path ini = azimuth_dir() / L"fh2-mods" / L"platform-popups.ini";
    if (GetPrivateProfileIntW(L"popups", L"WideText", 0, ini.c_str()) == 1) {
        g_wide = true;
        plog_f("WideText=1 (fh2-mods\\platform-popups.ini): pop-up text goes to the game as UTF-16, untested in game until T10 passes");
    }
    g_api = api;
    // The hooks only in a session where a mod declared pop-ups or tips (see the top of this file); the controller
    // poll only for tips.
    // A pop-up or tip sent from fh2mod_init waited in the queue until now; without a declaration it's dropped here.
    // Developer=1 counts as a declaration of both (ui_player.cpp): the test page and commands need the hooks.
    const bool developer = ui_developer();
    const bool declared  = developer || any_mod_uses(FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS);
    if (declared) want(developer || any_mod_uses(FH2_UI_USES_TIPS));
    g_prepared = true;
    if (!declared) off("no mod declared pop-ups or tips in fh2mod_init, so the game's notification code is left alone");
    else if (developer && !any_mod_uses(FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS))
        plog_f("no mod declared pop-ups or tips, but Developer=1 (fh2-mods\\platform-popups.ini) has the hooks go in for the UI templates' tests");
}

bool popups_track_page() { return g_track_wanted.load(); }

int popups_hud_on_screen() { return g_tracking.load() ? (g_on_screen.load() ? 1 : 0) : -1; }

void popups_note_callback(FH2Mod* m) {
    if (m) queue().note_callback(m, now_ms());
}

void popups_mod_faulted(FH2Mod* m) {
    if (m) queue().faulted(mod_ref(m));
}

void popups_set_platform_toasts(bool on) { queue().set_platform_toasts(on); }
void popups_set_mod_toasts(FH2Mod* m, bool on) { queue().set_mod_toasts(m, on); }
void popups_set_tips(bool on) { queue().set_tips(on); }
int popups_tips_again() { return queue().tips_again(); }
std::vector<PopupRecent> popups_recent() { return queue().recent(); }
std::string popups_describe() { return queue().describe(now_ms()); }
void popups_force_device(int keyboard) { g_force_device = keyboard < 0 ? -1 : keyboard ? 1 : 0; }
int popups_device() { return g_force_device.load() >= 0 ? g_force_device.load() : device_auto_kbd(); }
bool popups_started() { return g_started.load(); }

} // namespace fh2platform
