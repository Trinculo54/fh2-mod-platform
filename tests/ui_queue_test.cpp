// The pop-up queue's rules (src/popup_queue.cpp), with no game: the order, the pacing around the game's own
// pop-ups, replacing, the caps, expiry counted only while a pop-up could show, REPLY and URGENT, the player's
// switches, the text (capitals, cuts, ASCII folding), tips with their seen state, delays, spacing and buttons, and
// the log lines. A fake clock drives it, and a map stands in for fh2-mods\platform-seen.txt. Exits 0 when every
// check passes.
#include "popup_queue.hpp"

#include <windows.h>

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
void same(const std::string& got, const std::string& want, const char* what) {
    check(got == want, what);
    if (got != want) std::printf("     got  [%s]\n     want [%s]\n", got.c_str(), want.c_str());
}

std::vector<std::string> g_log;
std::map<std::string, std::string> g_seen;   // "who\tkey" -> value
struct Bind {
    uint32_t vk, mods, chord;
};
std::map<std::string, Bind> g_hotkeys;

bool logged(const std::string& part) {
    for (const std::string& l : g_log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
int count_logged(const std::string& part) {
    int n = 0;
    for (const std::string& l : g_log)
        if (l.find(part) != std::string::npos) ++n;
    return n;
}

PopupEnv env() {
    PopupEnv e;
    e.log   = [](const std::string& who, const std::string& line) { g_log.push_back("[" + who + "] " + line); };
    e.upper = [](const std::string& s) {
        std::string u = s;
        for (char& c : u)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        // the platform's capitals know Polish: ż to Ż, ó to Ó
        for (std::size_t i = 0; (i = u.find("\xC5\xBC", i)) != std::string::npos;) u.replace(i, 2, "\xC5\xBB");
        for (std::size_t i = 0; (i = u.find("\xC3\xB3", i)) != std::string::npos;) u.replace(i, 2, "\xC3\x93");
        for (std::size_t i = 0; (i = u.find("\xC4\x85", i)) != std::string::npos;) u.replace(i, 2, "\xC4\x84");   // a with ogonek
        for (std::size_t i = 0; (i = u.find("\xC4\x99", i)) != std::string::npos;) u.replace(i, 2, "\xC4\x98");   // e with ogonek
        for (std::size_t i = 0; (i = u.find("\xC5\x82", i)) != std::string::npos;) u.replace(i, 2, "\xC5\x81");   // l with stroke
        return u;
    };
    e.tr = [](const char* english) {
        const std::string s = english;
        return s == "Space" ? std::string("Spacja") : s;   // as the Polish table has it
    };
    e.seen_get = [](const std::string& who, const std::string& key) {
        const auto it = g_seen.find(who + "\t" + key);
        return it == g_seen.end() ? std::string() : it->second;
    };
    e.seen_set = [](const std::string& who, const std::string& key, const std::string& v) {
        g_seen[who + "\t" + key] = v;
        return true;
    };
    e.seen_clear      = [](const std::string& who, const std::string& key) { return g_seen.erase(who + "\t" + key) > 0; };
    e.seen_clear_tips = [] {
        int n = 0;
        for (auto it = g_seen.begin(); it != g_seen.end();)
            if (it->first.find("\ttip.") != std::string::npos) {
                it = g_seen.erase(it);
                ++n;
            } else {
                ++it;
            }
        return n;
    };
    e.hotkey = [](const void*, const std::string& key, uint32_t* vk, uint32_t* mods, uint32_t* chord) {
        const auto it = g_hotkeys.find(key);
        if (it == g_hotkeys.end()) return false;
        *vk    = it->second.vk;
        *mods  = it->second.mods;
        *chord = it->second.chord;
        return true;
    };
    e.key_name   = [](uint32_t vk) { return vk == 0xBA ? std::string(";") : vk == 0xE2 ? std::string("\xD0\x96") : std::string(); };
    e.clock_text = [] { return std::string("12:00"); };
    return e;
}

int g_a = 0, g_b = 0, g_c = 0, g_d = 0;   // the mods' identities
PopupMod mod(int* id, const char* name, uint32_t uses = FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS) {
    PopupMod m;
    m.id   = id;
    m.name = name;
    m.dll  = std::string(name) + ".dll";
    m.uses = uses;
    return m;
}
const PopupMod A = mod(&g_a, "ModA"), B = mod(&g_b, "ModB"), C = mod(&g_c, "ModC"), D = mod(&g_d, "ModD");

FH2Toast toast(const char* title, const char* line = nullptr, const char* key = nullptr, uint32_t flags = 0) {
    FH2Toast t{};
    t.size     = sizeof t;
    t.title    = title;
    t.subtitle = line;
    t.key      = key;
    t.flags    = flags;
    return t;
}
FH2Tip tip(const char* key, const char* text, uint32_t glyph = 0, uint32_t vk = 0) {
    FH2Tip t{};
    t.size         = sizeof t;
    t.key          = key;
    t.text         = text;
    t.button.glyph = glyph;
    t.button.vk    = vk;
    return t;
}

// The game as the glue sees it: on screen, the Notifications option on, a component ready.
struct Game {
    double now     = 0;
    bool on_screen = true, suspended = false, keyboard = false, wide = false, watch = true;
    int option     = 1;
    PopupOutcome outcome = PopupOutcome::shown;
    std::vector<PopupShow> shown;
};

// One tick: what the queue picks is "shown" at once, and the watch sees it.
PopupShow step(PopupQueue& q, Game& g) {
    PopupTick in;
    in.now       = g.now;
    in.on_screen = g.on_screen;
    in.suspended = g.suspended;
    in.option    = g.option;
    in.keyboard  = g.keyboard;
    in.ready     = true;
    in.wide      = g.wide;
    const PopupShow s = q.tick(in);
    if (s.id) {
        g.shown.push_back(s);
        if (g.watch && g.outcome == PopupOutcome::shown) q.show_seen(g.now);
        q.outcome(s.id, g.outcome, g.now);
    }
    return s;
}
// Ticks every 16 ms until `until`.
void run(PopupQueue& q, Game& g, double until) {
    while (g.now < until) {
        g.now += 16;
        step(q, g);
    }
}
// Ready, as the glue makes it once its hooks are in.
void arm(PopupQueue& q, const Game& g) {
    q.set_watch(g.watch);
    q.set_available(1);
}

void reset() {
    g_log.clear();
    g_seen.clear();
    g_hotkeys.clear();
}

} // namespace

int main() {
    // ---- text ----
    {
        bool changed = false;
        same(popup_fold_ascii("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84", &changed), "Zazolc gesla jazn",
             "folding: Polish letters to their base letters");
        check(changed, "folding: says it folded");
        same(popup_fold_ascii("\xC5\x81\xC3\xB3" "d\xC5\xBA, Stra\xC3\x9F" "e, \xC5\x92uvre, \xC3\x86on"), "Lodz, Strasse, OEuvre, AEon", "folding: L stroke, sharp s and ligatures");
        same(popup_fold_ascii("1\xC2\xA0" "234 \xE2\x80\x94 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x98so\xE2\x80\x99\xE2\x80\xA6"), "1 234 - \"quoted\" 'so'...",
             "folding: no-break space, dash, quotes, ellipsis");
        same(popup_fold_ascii("S\xC3\xA3o Paulo \xC2\xB7 20 G"), "Sao Paulo - 20 G", "folding: Portuguese, and the middle dot");
        same(popup_fold_ascii("\xE6\x97\xA5\xE6\x9C\xAC \xD0\x9C\xD0\xBE\xD0\xB4"), "?? ???", "folding: anything else is a question mark");
        same(popup_fold_ascii("bad \xFF byte"), "bad ? byte", "folding: a byte that isn't UTF-8");
        changed = true;
        same(popup_fold_ascii("Plain ASCII.", &changed), "Plain ASCII.", "folding: ASCII as it is");
        check(!changed, "folding: says nothing was folded");
    }

    // ---- before the hooks: queued; then shown, in capitals, cut ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        q.set_watch(true);
        FH2Toast t = toast("Snapshot written", "WreckingBall-defs.txt, next to the game", "Snap");
        t.icon     = FH2_ICON_TROPHY;
        check(q.show_toast(A, &t, g.now) == 1, "queued before the hooks are in");
        check(logged("[ModA] pop-up queued: Snapshot written / WreckingBall-defs.txt, next to the game (key Snap)"), "the queued line");
        run(q, g, 1000);
        check(g.shown.empty(), "nothing shows before the queue is ready");
        check((q.status(&g_a, false) & FH2_UI_TOASTS) == 0, "ui_status: no FH2_UI_TOASTS before the hooks");
        q.set_available(1);
        check((q.status(&g_a, true) & (FH2_UI_TOASTS | FH2_UI_KEYBOARD)) == (FH2_UI_TOASTS | FH2_UI_KEYBOARD), "ui_status: FH2_UI_TOASTS and KEYBOARD");
        run(q, g, 1100);
        check(g.shown.size() == 1, "shown once the queue is ready");
        if (!g.shown.empty()) {
            same(g.shown[0].title, "SNAPSHOT WRITTEN", "the title in capitals");
            same(g.shown[0].line, "WreckingBall-defs.txt, next to the game", "the line as given");
            same(g.shown[0].icon, "Achievement", "the trophy is the game's Achievement picture");
            same(g.shown[0].mod, "ModA", "the mod's name");
        }
        check(logged("[ModA] pop-up shown (game's look): SNAPSHOT WRITTEN / WreckingBall-defs.txt, next to the game (key Snap)"), "the shown line");
        check((q.status(&g_a, false) & FH2_UI_ON_SCREEN) != 0, "ui_status: FH2_UI_ON_SCREEN");
        // cut at 28 and 45 characters, folded to ASCII, with "..."
        FH2Toast l = toast("A title that is far too long for the slot", "A line that is much too long for the game's notification to hold");
        q.show_toast(A, &l, g.now);
        run(q, g, 7000);
        check(g.shown.size() == 2, "the long one is shown 5 s after the first");
        if (g.shown.size() == 2) {
            same(g.shown[1].title, "A TITLE THAT IS FAR TOO L...", "the title cut at 28 characters");
            same(g.shown[1].line, "A line that is much too long for the game'...", "the line cut at 45 characters");
        }
        // Polish: capitals made by the platform, then folded for the narrow strings; wide keeps them
        FH2Toast p = toast("Osi\xC4\x85gni\xC4\x99" "cie \xC5\xBC\xC3\xB3\xC5\x82te", "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
        q.show_toast(A, &p, g.now);
        run(q, g, 13000);
        if (g.shown.size() == 3) same(g.shown[2].title, "OSIAGNIECIE ZOLTE", "Polish: capitals, then folded");
        check(logged("[ModA] has text beyond ASCII in a pop-up"), "folding is logged once per mod");
        g.wide = true;
        q.show_toast(A, &p, g.now);
        run(q, g, 19000);
        if (g.shown.size() == 4) same(g.shown[3].line, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87", "wide text keeps its letters");
        FH2Toast w = toast("x", "\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85"
                                 "\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85"
                                 "\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85\xC4\x85");
        q.show_toast(A, &w, g.now);
        run(q, g, 25000);
        if (g.shown.size() == 5) check(g.shown[4].line.size() == 44 * 2 + 3 && g.shown[4].line.substr(88) == "\xE2\x80\xA6", "wide text is cut in characters, with an ellipsis");
    }

    // ---- pacing: 5.0 s after any Show the watch saw; 5.5 s after the platform's own without it ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        g.now        = 1000;
        q.show_seen(g.now);   // the game's own road discovery
        FH2Toast t = toast("One");
        q.show_toast(A, &t, g.now);
        run(q, g, 5900);
        check(g.shown.empty(), "waits 5 s after the game's own pop-up");
        run(q, g, 6100);
        check(g.shown.size() == 1, "then shows");
        check(logged("it waited") && logged("for the game's own pop-ups"), "the shown line says what it waited for");
        Game h;
        h.watch       = false;
        h.outcome     = PopupOutcome::unknown;
        PopupQueue q2{env()};
        arm(q2, h);
        FH2Toast a = toast("A"), b = toast("B");
        q2.show_toast(A, &a, 0);
        q2.show_toast(B, &b, 0);
        run(q2, h, 5400);
        check(h.shown.size() == 1, "without the watch: the second waits 5.5 s");
        run(q2, h, 5600);
        check(h.shown.size() == 2, "and then shows");
    }

    // ---- order: URGENT, then REPLY, then the rest; REPLY only within 1 s of a callback ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        q.show_seen(0);   // something on screen, so all three wait
        FH2Toast n = toast("Normal"), r = toast("Reply", nullptr, nullptr, FH2_TOAST_FLAG_REPLY), u = toast("Urgent", nullptr, nullptr, FH2_TOAST_FLAG_URGENT);
        q.show_toast(A, &n, 0);
        run(q, g, 4400);
        q.note_callback(&g_b, g.now);
        q.show_toast(B, &r, g.now + 100);   // shown within 6 s of time it could have shown, after the URGENT one
        q.show_toast(C, &u, g.now + 100);
        run(q, g, 16000);
        check(g.shown.size() == 3 && g.shown[0].title == "URGENT" && g.shown[1].title == "REPLY" && g.shown[2].title == "NORMAL",
              "URGENT first, then REPLY, then the rest");
        check(logged("[ModB] reply pop-up queued") && logged("[ModC] urgent pop-up queued"), "the queued lines name the kind");
        FH2Toast late = toast("Late reply", nullptr, nullptr, FH2_TOAST_FLAG_REPLY);
        q.show_toast(B, &late, 20000);
        check(logged("[ModB] sent a reply pop-up more than 1 s after"), "a REPLY 1 s or more after a callback is a normal one (logged)");
        check(logged("[ModB] pop-up queued: Late reply"), "and is queued as a normal one");
    }

    // ---- REPLY waits at most 6 s of time it could have shown ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        q.note_callback(&g_a, 0);
        FH2Toast r = toast("Reply", nullptr, "R", FH2_TOAST_FLAG_REPLY);
        q.show_toast(A, &r, 0);
        g.on_screen = false;   // a page callback: the pause menu is up for 20 s
        run(q, g, 20000);
        g.on_screen = true;
        for (int i = 0; i < 8; ++i) {   // then the game's own pop-ups, back to back
            q.show_seen(g.now);
            run(q, g, g.now + 1000);
        }
        check(g.shown.empty() && logged("pop-up dropped: a reply that couldn't show within 6 s"), "REPLY dropped after 6 s it could have shown");
    }

    // ---- URGENT: twice a session per mod ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Toast u   = toast("Fault", "Do this", nullptr, FH2_TOAST_FLAG_URGENT);
        for (int i = 0; i < 3; ++i) q.show_toast(A, &u, 0);
        check(count_logged("[ModA] urgent pop-up queued") == 1 && logged("has sent more than 2 urgent pop-ups this session"),
              "a 3rd URGENT is a normal one, logged once");
    }

    // ---- replace while waiting; the same one on screen isn't queued again ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        q.show_seen(0);
        FH2Toast a = toast("First", nullptr, "K"), b = toast("Other"), c = toast("Second", nullptr, "K");
        q.show_toast(A, &a, 0);
        q.show_toast(A, &b, 0);
        q.show_toast(A, &c, 100);
        check(q.waiting() == 2, "the same key replaces the waiting one");
        check(logged("[ModA] pop-up replaced while waiting: Second (key K)"), "the replaced line");
        run(q, g, 5100);
        check(g.shown.size() == 1 && g.shown[0].title == "SECOND", "replaced where it stood: shown first");
        FH2Toast again = toast("Second", nullptr, "K");
        check(q.show_toast(A, &again, g.now) == 1 && q.waiting() == 1, "the same one on screen isn't queued again (returns 1)");
        check(logged("pop-up not queued again: the same one is on screen now"), "the not-queued-again line");
        FH2Toast changed = toast("Third", nullptr, "K");
        q.show_toast(A, &changed, g.now);
        check(q.waiting() == 2, "a new text with the key of the one on screen waits its turn");
    }

    // ---- 4 waiting per mod: a 5th drops the oldest that isn't URGENT ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        q.show_seen(0);
        FH2Toast u = toast("U", nullptr, "u", FH2_TOAST_FLAG_URGENT), t1 = toast("T1", nullptr, "t1"), t2 = toast("T2", nullptr, "t2"),
                 t3 = toast("T3", nullptr, "t3"), t4 = toast("T4", nullptr, "t4");
        for (FH2Toast* t : {&u, &t1, &t2, &t3, &t4}) q.show_toast(A, t, 0);
        check(q.waiting() == 4 && logged("pop-up dropped: a 5th was sent while 4 were waiting (T1 (key t1))"), "the oldest non-URGENT one goes");
        FH2Toast other = toast("Other mod");
        q.show_toast(B, &other, 0);
        check(q.waiting() == 5, "another mod's pop-ups don't count toward it");
    }

    // ---- expiry: only time the pop-up could have shown ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Toast t   = toast("Expires", nullptr, "E");
        t.expire_s   = 2;
        g.on_screen  = false;
        q.show_toast(A, &t, 0);
        q.show_seen(0);
        run(q, g, 30000);   // the pause menu: nothing counted
        check(q.waiting() == 1, "time off screen doesn't count");
        g.suspended = true;
        g.on_screen = true;
        run(q, g, 60000);
        check(q.waiting() == 1, "nor while the game's HUD is suspended");
        check((q.status(&g_a, false) & FH2_UI_GAME_HUD_HIDDEN) != 0, "ui_status: FH2_UI_GAME_HUD_HIDDEN");
        g.suspended = false;
        for (int i = 0; i < 3; ++i) {   // on screen, behind the game's own
            q.show_seen(g.now);
            run(q, g, g.now + 1000);
        }
        check(q.waiting() == 0 && logged("pop-up dropped: it could have shown for 2 s and didn't (Expires (key E))"), "waiting behind others counts");
        FH2Toast big = toast("Big");
        big.expire_s = 9999;
        q.show_toast(A, &big, g.now);
        check(logged("expire_s is at most 600"), "expire_s over 600 is 600");
    }

    // ---- the player's switches and the game's option ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Toast t   = toast("Hello", "there", "H");
        q.set_platform_toasts(false);
        check(q.show_toast(A, &t, 0) == 0 && logged("pop-up not shown: the player turned mod pop-ups off (Hello / there (key H))"), "Mod pop-ups off");
        check((q.status(&g_a, false) & FH2_UI_TOASTS_OFF) != 0, "ui_status: FH2_UI_TOASTS_OFF");
        q.set_platform_toasts(true);
        q.set_mod_toasts(&g_a, false);
        check(q.show_toast(A, &t, 0) == 0 && logged("the player turned this mod's pop-ups off"), "this mod's Pop-ups off");
        check(q.show_toast(B, &t, 0) == 1, "another mod's still show");
        q.set_mod_toasts(&g_a, true);
        g.option = 0;
        step(q, g);
        check(q.waiting() == 0 && logged("pop-up dropped: the game's Notifications setting is off"), "waiting ones drop when the game's option is off");
        check(q.show_toast(A, &t, 1) == 0 && logged("pop-up not shown: the game's Notifications setting is off"), "and new ones aren't queued");
        check((q.status(&g_a, false) & FH2_UI_TOASTS_OFF) != 0, "ui_status: FH2_UI_TOASTS_OFF with the game's option off");
        g.option = -1;
        step(q, g);
        check(q.show_toast(A, &t, 2) == 1, "an option that can't be read counts as on");
        q.set_tips(false);
        check((q.status(&g_a, false) & FH2_UI_TIPS_OFF) != 0, "ui_status: FH2_UI_TIPS_OFF");
    }

    // ---- what a mod passes ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Toast t   = toast("x", nullptr, "bad key!");
        check(q.show_toast(A, &t, 0) == 0 && logged("isn't a usable key"), "a bad key: 0");
        FH2Toast n = toast("");
        check(q.show_toast(A, &n, 0) == 0 && logged("a pop-up needs a title"), "no title: 0");
        FH2Toast s = toast("x");
        s.size     = 4;
        check(q.show_toast(A, &s, 0) == 0 && logged("the FH2Toast's size is too small"), "a size too small: 0");
        check(q.show_toast(A, nullptr, 0) == 0, "no FH2Toast: 0");
        FH2Toast old = toast("Older header", "line");
        old.icon     = FH2_ICON_ROADS;
        old.size     = static_cast<uint32_t>(offsetof(FH2Toast, icon));   // a header without icon and the rest
        check(q.show_toast(A, &old, 0) == 1, "only `size` bytes are read");
        FH2Toast snd = toast("Sound");
        snd.sound    = FH2_SOUND_NOTIFY;
        snd.icon     = 15;
        q.show_toast(B, &snd, 0);
        check(logged("FH2_SOUND_NOTIFY isn't played with a pop-up") && logged("isn't one of the game's nine pictures"), "NOTIFY and an API 11 icon are ignored, logged");
        FH2Toast shutter = toast("Shutter");
        shutter.sound    = FH2_SOUND_SHUTTER;
        q.show_toast(C, &shutter, 0);
        run(q, g, 11000);
        check(g.shown.size() == 3 && g.shown[0].icon.empty() && g.shown[1].sound == 0 && g.shown[1].icon.empty() && g.shown[2].sound == FH2_SOUND_SHUTTER,
              "the older header's icon isn't read; NOTIFY isn't played; another sound is");
        PopupMod undeclared = mod(&g_d, "ModD", 0);
        FH2Toast u          = toast("Undeclared");
        q.show_toast(undeclared, &u, g.now);
        q.show_toast(undeclared, &u, g.now);
        check(count_logged("[ModD] shows pop-ups without saying so") == 1, "pop-ups without ui_uses: shown, and logged once");
    }

    // ---- clear_toast, faults, and the session going off ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        q.show_seen(0);
        FH2Toast a = toast("A", nullptr, "a"), b = toast("B", nullptr, "b"), c = toast("C");
        q.show_toast(A, &a, 0);
        q.show_toast(A, &b, 0);
        q.show_toast(A, &c, 0);
        q.clear_toast(A, "a");
        check(q.waiting() == 2 && logged("pop-up taken down while waiting (key a)"), "clear_toast(key)");
        q.clear_toast(A, nullptr);
        check(q.waiting() == 0 && logged("2 waiting pop-ups taken down"), "clear_toast(NULL): all of the mod's");
        q.show_toast(A, &a, 0);
        q.show_toast(B, &b, 0);
        FH2Tip t = tip("T", "A tip");
        q.show_tip(A, &t, 0);
        q.faulted(A);
        check(q.waiting() == 1 && logged("[ModA] ModA faulted, so its waiting pop-ups were dropped (2)"), "a fault drops the mod's pop-ups and tip");
        q.set_available(0, "the game's notification code isn't as expected");
        check(q.waiting() == 0 && logged("[ModB] pop-up dropped: the game's notification code isn't as expected (B (key b))"), "off: everything waiting goes");
        check(q.show_toast(A, &a, 0) == 0 && q.show_tip(A, &t, 0) == 0, "off: later calls return 0");
    }

    // ---- private text and the rate limit ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Toast p   = toast("Signed in as", "SecretGamertag", "SignIn", FH2_TOAST_FLAG_PRIVATE);
        q.show_toast(A, &p, 0);
        run(q, g, 100);
        check(!logged("SecretGamertag") && logged("[ModA] pop-up shown (game's look, private): key SignIn"), "PRIVATE: the log has the key only");
        check(!q.recent().empty() && q.recent()[0].title == "(private)" && q.recent()[0].when == "12:00", "Recent pop-ups: \"(private)\"");
        q.show_seen(g.now);
        for (int i = 0; i < 20; ++i) {
            const std::string title = "Count " + std::to_string(i);
            FH2Toast c              = toast(title.c_str(), nullptr, "Count");
            q.show_toast(A, &c, g.now + i * 100);
        }
        check(count_logged("pop-up replaced while waiting") == 1, "replacing: one line in 10 s");
        g.now += 11000;
        FH2Toast c = toast("Count 20", nullptr, "Count");
        q.show_toast(A, &c, g.now);
        check(logged("[ModA] pop-ups replaced 18 more times in the last 10 s (key Count)"), "then the count of the ones held back");
    }

    // ---- recent: the last 10, newest first ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        for (int i = 0; i < 12; ++i) {
            const std::string title = "P" + std::to_string(i);
            FH2Toast t              = toast(title.c_str());
            q.show_toast(A, &t, g.now);
            run(q, g, g.now + 5100);
        }
        const auto r = q.recent();
        check(r.size() == 10 && r[0].title == "P11" && r[9].title == "P2" && r[0].mod == "ModA", "Recent pop-ups: the last 10, newest first");
        check(q.describe(g.now).find("nothing waiting") != std::string::npos, "describe(): nothing waiting");
    }

    // ---- tips: seen state, delay, the announcement, the button ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Tip t     = tip("FirstTree", "Press {button} to keep the tree", FH2_GLYPH_A, VK_SPACE);
        check(q.show_tip(A, &t, 0) == 1 && logged("[ModA] tip FirstTree queued, to show after 60 s of driving: Press {button} to keep the tree"), "a tip is queued");
        run(q, g, 59000);
        check(g.shown.empty(), "not before 60 s of driving (delay_s 0)");
        run(q, g, 61000);
        check(g.shown.size() == 1, "after 60 s of driving");
        if (!g.shown.empty()) {
            same(g.shown[0].title, "MODA", "a tip's title is its mod's name in capitals");
            same(g.shown[0].line, "Press (A) to keep the tree", "the pad's glyph, written as text");
            check(g.shown[0].icon.empty(), "a tip has no icon");
        }
        check(g_seen["ModA.dll\ttip.FirstTree"] == "seen" && logged("[ModA] tip FirstTree shown, now seen: Press (A) to keep the tree"), "seen once drawn");
        check(q.tip_seen(A, "FirstTree") == 1, "tip_seen: 1");
        check(q.show_tip(A, &t, g.now) == 0 && logged("tip FirstTree not shown: it was seen already"), "a seen tip isn't shown again");
        q.set_tip_seen(A, "FirstTree", 0);
        check(q.tip_seen(A, "FirstTree") == 0 && logged("tip FirstTree marked not seen"), "set_tip_seen(0) brings it back");
        check(q.show_tip(A, &t, g.now) == 0 && logged("this mod has shown a tip this session already"), "but one tip a mod a session");
        // the keyboard: the key, in the session's language (Polish here)
        Game k;
        k.keyboard     = true;
        PopupQueue q2{env()};
        arm(q2, k);
        FH2Tip space   = tip("Space", "Press {button} to keep it", FH2_GLYPH_A, VK_SPACE);
        space.delay_s  = 1;
        q2.show_tip(B, &space, 0);
        run(q2, k, 11000);
        check(k.shown.empty(), "never in the first 12 s the HUD is on screen (the platform's notice)");
        run(q2, k, 12100);
        if (k.shown.size() == 1) same(k.shown[0].line, "Press [SPACJA] to keep it", "the keyboard: the key's name, in capitals, in the game's language");
        else check(false, "the keyboard tip shows after 12 s");
    }

    // ---- tips: spacing, caps and pop-ups first ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Tip a = tip("TipA", "Tip from A"), b = tip("TipB", "Tip from B"), c = tip("TipC", "Tip from C"), d = tip("TipD", "Tip from D");
        for (FH2Tip* t : {&a, &b, &c, &d}) t->delay_s = 1;
        q.show_tip(A, &a, 0);
        q.show_tip(B, &b, 0);
        q.show_tip(C, &c, 0);
        q.show_seen(0);   // the game's own pop-up: the lane is busy until 5 s
        FH2Toast pop = toast("A pop-up");
        q.show_toast(D, &pop, 0);
        run(q, g, 11000);
        check(g.shown.size() == 1 && !g.shown[0].tip, "a pop-up goes before a tip");
        run(q, g, 30000);
        check(g.shown.size() == 2 && g.shown[1].tip && g.shown[1].line == "Tip from A", "then the oldest tip");
        const double first = 12016;   // 12 s after the HUD was first on screen, at 16 ms
        run(q, g, first + 170000);
        check(g.shown.size() == 2, "the next tip waits 3 minutes");
        run(q, g, first + 181000);
        check(g.shown.size() == 3 && g.shown[2].line == "Tip from B", "then the next");
        run(q, g, first + 362000);
        check(g.shown.size() == 4 && g.shown[3].line == "Tip from C", "and the third");
        check(q.show_tip(D, &d, g.now) == 0 && logged("three tips have shown this session already"), "three tips a session in all");
    }

    // ---- tips: what the button needs; hotkeys and chords ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Tip no_slot = tip("NoSlot", "Press the button", FH2_GLYPH_A);
        check(q.show_tip(A, &no_slot, 0) == 0 && logged("has a button, but its text has no {button}"), "a button without {button}: 0");
        FH2Tip no_button = tip("NoButton", "Press {button}");
        check(q.show_tip(A, &no_button, 0) == 0 && logged("has {button}, but the tip has no button"), "{button} without a button: 0");
        g_hotkeys["Snap"] = {'Z', FH2_KEY_CTRL, FH2_PAD_LB | FH2_PAD_RB | FH2_PAD_X};
        g_hotkeys["Many"] = {0, 0, FH2_PAD_LB | FH2_PAD_RB | FH2_PAD_X | FH2_PAD_DPAD_UP};
        g_hotkeys["Off"]  = {0, 0, 0};
        FH2Tip hk         = tip("Hot", "Press {button} for a snapshot");
        hk.button.hotkey  = "Snap";
        hk.delay_s        = 1;
        q.show_tip(A, &hk, 0);
        g.keyboard = true;
        run(q, g, 13000);
        if (g.shown.size() == 1) same(g.shown[0].line, "Press [CTRL] + [Z] for a snapshot", "a hotkey's key with its modifier");
        Game p;
        PopupQueue q2{env()};
        arm(q2, p);
        FH2Tip pad    = hk;
        pad.key       = "HotPad";   // "Hot" was seen above
        q2.show_tip(A, &pad, 0);
        FH2Tip many      = tip("Many", "Hold {button}");
        many.button.hotkey = "Many";
        many.delay_s       = 1;
        q2.show_tip(B, &many, 0);
        FH2Tip off      = tip("Off", "Press {button}");
        off.button.hotkey = "Off";
        off.delay_s       = 1;
        q2.show_tip(C, &off, 0);
        run(q2, p, 13000 + 3 * 181000);
        check(p.shown.size() == 2, "a tip whose hotkey is bound to nothing isn't shown");
        if (p.shown.size() == 2) {
            same(p.shown[0].line, "Press (LB) + (RB) + (X) for a snapshot", "a hotkey's chord as glyphs");
            same(p.shown[1].line, "Hold (LB) + (RB) + (X) + ...", "a fourth button as an ellipsis");
        }
        check(logged("tip Off dropped: its button can't be written"), "the dropped line");
        // key names
        Game kk;
        kk.keyboard   = true;
        PopupQueue q3{env()};
        arm(q3, kk);
        FH2Tip semi   = tip("Semi", "Press {button}", 0, 0xBA);
        semi.delay_s  = 1;
        q3.show_tip(A, &semi, 0);
        FH2Tip cyr   = tip("Cyr", "Press {button}", 0, 0xE2);
        cyr.delay_s  = 1;
        q3.show_tip(B, &cyr, 0);
        FH2Tip arrow = tip("Arrow", "Press {button}", FH2_GLYPH_DPAD_RIGHT, VK_RIGHT);
        arrow.delay_s = 1;
        q3.show_tip(C, &arrow, 0);
        run(q3, kk, 13000 + 2 * 181000);
        if (kk.shown.size() == 3) {
            same(kk.shown[0].line, "Press [;]", "a key outside the table: its name on the layout");
            same(kk.shown[1].line, "Press [0xE2]", "a name that isn't ASCII: the key's code");
            same(kk.shown[2].line, "Press [RIGHT]", "an arrow: the platform's word");
        } else {
            check(false, "the three key tips show");
        }
        Game pp;
        PopupQueue q4{env()};
        arm(q4, pp);
        q4.show_tip(A, &arrow, 0);
        run(q4, pp, 13000);
        if (pp.shown.size() == 1) same(pp.shown[0].line, "Press (D-PAD RIGHT)", "the pad: a d-pad glyph with the platform's word");
    }

    // ---- tips: seen only when known to be drawn; UNTIL_DONE ----
    reset();
    {
        Game g;
        g.watch       = false;
        g.outcome     = PopupOutcome::unknown;
        g.option      = -1;
        PopupQueue q{env()};
        arm(q, g);
        FH2Tip t      = tip("Maybe", "A tip");
        t.delay_s     = 1;
        q.show_tip(A, &t, 0);
        run(q, g, 13000);
        check(g.shown.size() == 1 && g_seen.count("ModA.dll\ttip.Maybe") == 0 && logged("whether the game drew it can't be told"),
              "no watch and the option unread: not marked seen");
        Game r;
        r.outcome     = PopupOutcome::refused;
        PopupQueue q2{env()};
        arm(q2, r);
        q2.show_tip(A, &t, 0);
        run(q2, r, 13000);
        check(g_seen.count("ModA.dll\ttip.Maybe") == 0 && logged("tip Maybe not shown: the game didn't draw it"), "refused by the game: not seen");
        // UNTIL_DONE: counted per session, seen only when the mod says
        FH2Tip u       = tip("Menu", "Mod settings: Pause > MODS");
        u.flags        = FH2_TIP_FLAG_UNTIL_DONE;
        u.max_sessions = 2;
        u.delay_s      = 1;
        for (int session = 1; session <= 3; ++session) {
            Game s;
            PopupQueue qs{env()};
        arm(qs, s);
            const int r1  = qs.show_tip(B, &u, 0);
            run(qs, s, 13000);
            if (session < 3) check(r1 == 1 && s.shown.size() == 1, "UNTIL_DONE: shown in each session up to max_sessions");
            else check(r1 == 0 && logged("tip Menu not shown: it has shown in 2 sessions, as many as its max_sessions"), "then no more");
        }
        check(g_seen["ModB.dll\ttip.Menu"] == "2", "the sessions are counted in the store");
        PopupQueue q3{env()};
        check(q3.tip_seen(B, "Menu") == 0, "UNTIL_DONE: not seen until the mod says");
        q3.set_tip_seen(B, "Menu", 1);
        check(q3.tip_seen(B, "Menu") == 1 && g_seen["ModB.dll\ttip.Menu"] == "seen", "set_tip_seen(1): seen");
        // set_tip_seen drops a waiting tip
        Game w;
        PopupQueue q4{env()};
        arm(q4, w);
        FH2Tip x      = tip("Wait", "Waiting tip");
        q4.show_tip(C, &x, 0);
        q4.set_tip_seen(C, "Wait", 1);
        check(q4.waiting() == 0 && logged("tip Wait marked seen; it was waiting, so it won't show"), "set_tip_seen(1) drops it if it's waiting");
        g_seen["ModD.dll\ttip.Old"] = "seen";
        check(q4.tips_again() == 3 && g_seen.empty() && logged("[Pop-ups] every mod's tips can show again (3 forgotten)"), "Show tips again");
        q4.set_tips(false);
        check(q4.show_tip(D, &x, 0) == 0 && logged("not shown: the player turned tips off"), "Tips off: 0");
    }

    // ---- a tip's drive time counts only with nothing else showing ----
    reset();
    {
        Game g;
        PopupQueue q{env()};
        arm(q, g);
        FH2Tip t     = tip("Drive", "A tip");
        t.delay_s    = 20;
        q.show_tip(A, &t, 0);
        for (int i = 0; i < 20; ++i) {   // the game's own pop-ups every 4 s for 80 s
            q.show_seen(g.now);
            run(q, g, g.now + 4000);
        }
        check(g.shown.empty(), "no drive time while the game's pop-ups show");
        g.on_screen = false;
        run(q, g, g.now + 60000);
        check(g.shown.empty(), "nor off screen");
        g.on_screen = true;
        run(q, g, g.now + 21000);
        check(g.shown.size() == 1, "then 20 s of quiet driving");
    }

    std::printf(g_failed ? "%d FAILED\n" : "ALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
