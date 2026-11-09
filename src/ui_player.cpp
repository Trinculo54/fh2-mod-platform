// The player's controls for pop-ups and tips, and the UI templates' developer tools (platform 2.9.0; the design's
// sections 1.6, 1.10 and 5.4, with decisions D4, D13 and D15 as it recommends them).
//
// What the player sees:
//  - "FH2 Mod Platform", the last line of YOUR MODS's list of mods, opens the platform's own page: "Mod pop-ups"
//    (Same as Notifications / Off: the game's own notification only shows while its Notifications option is on, so
//    the platform can't offer more), "Tips" (On / Off), "Show tips again" (A: every mod's tips are unseen) and
//    "Recent pop-ups" (a page of the platform's own: the last 10 pop-ups and tips, newest first, with the mod that
//    sent each, "(private)" for a private one). "Tips: Off" leaves the platform's own first-run cues alone, and those
//    stay in the HUD notice's spot rather than becoming pop-ups (D15).
//  - On a mod's own page, above Load this mod: "Pop-ups" (On / Off), for a mod that declared FH2_UI_USES_TOASTS
//    without FH2_UI_USES_OWN_SWITCH.
//  settings.cpp lays the rows out; their text, values and effects are here. A change takes effect at once and is kept
//  in fh2-mods\platform-popups.ini ([player] and [mods], beside [popups] WideText), written on a thread of its own,
//  as the platform keeps its other choices in fh2-mods\platform-*.txt.
//
// For developers:
//  - "fh2mod Platform ui": what's on screen and waiting, and why (popups_describe(), private text hidden), and the
//    player's choices; "ui recent" lists Recent pop-ups. The command is added quietly: it never starts the command pipe
//    by itself, so it works whenever a mod's command has started it.
//  - With Developer=1 in [popups]: the pipe starts anyway, the pop-up hooks go in even when no mod declared pop-ups
//    (popups.cpp), the ui command's tests work (toast, tip, keyboard, pad, auto, demo, set), and the platform's page
//    gets "UI templates", a page whose rows fire each template once. Players never see either, so their text is
//    English.
#include "ui_player.hpp"

#include "lang.hpp"
#include "platform.hpp"
#include "popup_queue.hpp"
#include "popups.hpp"
#include "text.hpp"
#include "ui_templates.hpp"
#include "updates.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace fh2platform {
namespace {

namespace fs = std::filesystem;

const char* const kWho = "Pop-ups";   // the log's tag for the player's choices, as the queue's own lines

const FH2ModApi* g_api = nullptr;
FH2Mod g_self;                           // "Platform"
std::atomic<bool> g_developer{false};
std::atomic<bool> g_mod_popups{true};    // "Mod pop-ups": Same as Notifications (true), or Off
std::atomic<bool> g_tips{true};
std::mutex g_mu;
std::map<std::wstring, bool> g_mod_on;   // [mods]: each mod's "Pop-ups" row by its DLL's file name, lower case
std::mutex g_write_mu;                   // one write of the file at a time

fs::path ini_path() { return azimuth_dir() / L"fh2-mods" / L"platform-popups.ini"; }

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// ---- the file -----------------------------------------------------------------------------------------------

// What a new platform-popups.ini starts with: what each key does.
constexpr const char kIniHeader[] =
    "; FH2 Mod Platform: pop-ups and tips (platform 2.9.0). The platform writes [player] and [mods] when you change\r\n"
    "; their rows in game: Pause > MODS > YOUR MODS > FH2 Mod Platform, and each mod's own page.\r\n"
    "[player]\r\n"
    "; ModPopups: 1 = mods' pop-ups show while the game's Notifications option is on, 0 = no mod shows pop-ups\r\n"
    "; Tips: 1 = mods' tips show, each once; 0 = no mod shows tips\r\n"
    "[mods]\r\n"
    "; <a mod's DLL>=0 turns that mod's pop-ups off (the Pop-ups row on its page)\r\n"
    "[popups]\r\n"
    "; Developer=1 adds the UI templates test page and the test commands of \"fh2mod Platform ui\"\r\n"
    "; WideText=1 sends pop-up text to the game as UTF-16 (not yet seen working in game)\r\n";

// Writes the player's choices as they are now, on a thread of its own (a row's on_change runs on the game's UI
// thread). Each write has the latest values, so writes that finish out of order still leave the last choice.
void save() {
    std::thread([] {
        std::scoped_lock lk{g_write_mu};
        const fs::path ini = ini_path();
        std::error_code ec;
        if (!fs::exists(ini, ec))
            if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
                std::fputs(kIniHeader, f);
                std::fclose(f);
            }
        bool ok = WritePrivateProfileStringW(L"player", L"ModPopups", g_mod_popups ? L"1" : L"0", ini.c_str()) &&
                  WritePrivateProfileStringW(L"player", L"Tips", g_tips ? L"1" : L"0", ini.c_str());
        std::map<std::wstring, bool> mods;
        {
            std::scoped_lock lk2{g_mu};
            mods = g_mod_on;
        }
        for (const auto& [dll, on] : mods) ok = WritePrivateProfileStringW(L"mods", dll.c_str(), on ? L"1" : L"0", ini.c_str()) && ok;
        if (!ok)
            log_line(kWho, "couldn't write fh2-mods\\platform-popups.ini (error " + std::to_string(GetLastError()) +
                               "), so the choice holds only until the game ends");
    }).detach();
}

// [mods]: "<dll>=0" or "=1" lines.
void read_mods(const fs::path& ini) {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(L"mods", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    std::scoped_lock lk{g_mu};
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += std::wcslen(p) + 1) {
        const std::wstring line = p;
        const std::size_t eq    = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0 || line[0] == L';') continue;
        std::wstring dll = line.substr(0, eq), value = line.substr(eq + 1);
        while (!dll.empty() && dll.back() == L' ') dll.pop_back();
        while (!value.empty() && value.front() == L' ') value.erase(value.begin());
        g_mod_on[lower(dll)] = value.empty() || value[0] != L'0';
    }
}

// ---- the command: "fh2mod Platform ui" ----------------------------------------------------------------------

// The words of a command line, a word in double quotes kept whole.
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        if (s[i] == '"') {
            std::size_t e = s.find('"', i + 1);
            if (e == std::string::npos) e = s.size();
            out.push_back(s.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            std::size_t e = s.find(' ', i);
            if (e == std::string::npos) e = s.size();
            out.push_back(s.substr(i, e - i));
            i = e;
        }
    }
    return out;
}

std::string trimmed(const std::string& s) {
    const std::size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// A test command's arguments: "Title" "Line" options, each text in double quotes; or Title | Line | options, which
// needs no quotes (fh2mod.exe joins its arguments with spaces, and a shell takes the quotes away). The first two
// fields are kept whole; the rest are words.
std::vector<std::string> fields(const std::string& rest) {
    if (rest.find('|') == std::string::npos) return words(rest);
    std::vector<std::string> out;
    std::size_t at = 0;
    for (int n = 0; n < 2 && at <= rest.size(); ++n) {
        const std::size_t bar = rest.find('|', at);
        out.push_back(trimmed(rest.substr(at, bar == std::string::npos ? std::string::npos : bar - at)));
        at = bar == std::string::npos ? rest.size() + 1 : bar + 1;
    }
    if (at <= rest.size())
        for (const std::string& w : words(rest.substr(at)))
            if (w != "|") out.push_back(w);
    return out;
}

bool same_word(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

std::string device_text(int d) { return d == 1 ? "the keyboard" : "the controller"; }

// The player's choices, for "ui".
std::string choices_text() {
    std::string s = std::string("the player's choices (fh2-mods\\platform-popups.ini): mod pop-ups ") +
                    (g_mod_popups ? "same as Notifications" : "off") + ", tips " + (g_tips ? "on" : "off") + "\n";
    {
        std::scoped_lock lk{g_mu};
        for (const auto& [dll, on] : g_mod_on)
            if (!on) s += narrow(dll) + "'s pop-ups: off\n";
    }
    const int device = popups_device();
    s += device < 0 ? std::string("buttons in tips are written for the controller: no key has been pressed yet\n")
                    : "buttons in tips are written for " + device_text(device) + "\n";
    s += g_developer ? "Developer=1: the UI templates test page and the test commands are on\n"
                     : "the test commands (toast, tip, keyboard, pad, auto, demo, set) need Developer=1 in [popups]\n";
    return s;
}

std::string recent_text() {
    std::string s;
    for (const RecentRow& r : recent_page(popups_recent(), [](const char* english) { return tr(english); }))
        s += r.label + (r.value.empty() ? "" : "  " + r.value) + "  " + r.description + "\n";
    return s;
}

uint32_t icon_named(const std::string& w) {
    const char* const names[] = {"none", "trophy", "roads", "photo", "boards", "bucket", "carmeet", "club", "payout", "barnfind"};
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (same_word(w, names[i])) return i;
    return 0xFFFFFFFFu;
}

// A test pop-up from the platform (the key "Test", so the next one replaces it while it waits).
int test_toast(const char* title, const char* line, uint32_t icon, uint32_t flags, const char* key = "Test") {
    FH2Toast t{};
    t.size     = sizeof t;
    t.key      = key;
    t.title    = title;
    t.subtitle = line;
    t.icon     = icon;
    t.flags    = flags;
    return g_api->show_toast(&g_self, &t);
}

// A test tip with a button: `button` is A, B, X, Y, LB or RB, with a key for the keyboard.
int test_tip(const char* text, const std::string& button) {
    struct B {
        const char* name;
        uint32_t glyph, vk;
    };
    const B buttons[] = {{"A", FH2_GLYPH_A, VK_SPACE}, {"B", FH2_GLYPH_B, VK_ESCAPE}, {"X", FH2_GLYPH_X, 'X'},
                         {"Y", FH2_GLYPH_Y, 'Y'},      {"LB", FH2_GLYPH_LB, 'Q'},     {"RB", FH2_GLYPH_RB, 'E'}};
    FH2Tip t{};
    t.size    = sizeof t;
    t.text    = text;
    t.delay_s = 5;
    for (const B& b : buttons)
        if (same_word(button, b.name)) {
            t.button.glyph = b.glyph;
            t.button.vk    = b.vk;
        }
    return g_api->show_tip(&g_self, &t);
}

// Every 2.9.0 template in turn, each saying what it is.
std::string demo() {
    int queued = test_toast("Demo: a pop-up", "1 of 5: the commonest kind", FH2_ICON_TROPHY, 0, "Demo1");
    queued += test_toast("Demo: a reply", "2 of 5: answers a press, before the rest", FH2_ICON_PHOTO, FH2_TOAST_FLAG_REPLY, "Demo2");
    queued += test_toast("Demo: urgent", "3 of 5: a fault, first in the queue", FH2_ICON_NONE, FH2_TOAST_FLAG_URGENT, "Demo3");
    queued += test_toast("Demo: private", "4 of 5: the log has its key only", FH2_ICON_CLUB, FH2_TOAST_FLAG_PRIVATE, "Demo4");
    const int tip   = test_tip("5 of 5: a tip. Press {button} to try it", "A");
    const int sound = g_api->play_ui_sound(&g_self, FH2_SOUND_SELECT);
    return "demo: " + std::to_string(queued) + " of 4 pop-ups queued (urgent first, then the reply, then the others), the tip " +
           (tip ? "queued" : "not queued") + ", the sound " + (sound ? "played" : "not played") +
           "; fh2-mods\\platform.log says why for any that weren't. They show once you're driving.\n";
}

std::string run_ui(const std::string& args) {
    const std::string line = trimmed(args);
    if (line.empty()) return popups_describe() + choices_text();
    const std::size_t space = line.find(' ');
    const std::string sub   = line.substr(0, space);
    std::vector<std::string> w{sub};
    if (space != std::string::npos)
        for (const std::string& f : fields(line.substr(space + 1))) w.push_back(f);
    if (same_word(sub, "recent")) return recent_text();
    const char* const tests[] = {"toast", "tip", "keyboard", "pad", "auto", "demo", "set"};
    bool test = false;
    for (const char* t : tests) test = test || same_word(sub, t);
    if (!test)
        return "error: ui has no \"" + sub + "\": ui, ui recent, and with Developer=1 ui toast Title | Line | [icon] [urgent] [reply] "
               "[private], ui tip Press {button} to try it | [A, B, X, Y, LB or RB], ui keyboard, ui pad, ui auto, ui demo, "
               "ui set ModPopups|Tips|<mod> on|off\n";
    if (!g_developer) return "error: ui " + sub + " needs Developer=1 in fh2-mods\\platform-popups.ini's [popups], then a restart of the game\n";
    if (same_word(sub, "set")) {   // a row changed as the player would: Mod pop-ups, Tips or a mod's Pop-ups
        const std::vector<std::string> sw = words(space == std::string::npos ? "" : line.substr(space + 1));
        if (sw.size() != 2 || !(same_word(sw[1], "on") || same_word(sw[1], "off"))) return "error: ui set ModPopups|Tips|<mod> on|off\n";
        return settings_player_set(sw[0], same_word(sw[1], "on"));
    }
    if (same_word(sub, "toast")) {
        if (w.size() < 2 || w[1].empty()) return "error: ui toast Title | Line | [icon] [urgent] [reply] [private]\n";
        uint32_t icon = FH2_ICON_TROPHY, flags = 0;
        for (std::size_t i = 3; i < w.size(); ++i) {
            if (same_word(w[i], "urgent")) flags |= FH2_TOAST_FLAG_URGENT;
            else if (same_word(w[i], "reply")) flags |= FH2_TOAST_FLAG_REPLY;
            else if (same_word(w[i], "private")) flags |= FH2_TOAST_FLAG_PRIVATE;
            else if (const uint32_t n = icon_named(w[i]); n != 0xFFFFFFFFu) icon = n;
            else return "error: \"" + w[i] + "\" isn't an icon (none, trophy, roads, photo, boards, bucket, carmeet, club, payout, barnfind) or a flag\n";
        }
        return test_toast(w[1].c_str(), w.size() > 2 && !w[2].empty() ? w[2].c_str() : nullptr, icon, flags)
                   ? "queued: it shows once you're driving\n"
                   : "error: not queued; fh2-mods\\platform.log says why\n";
    }
    if (same_word(sub, "tip")) {
        if (w.size() < 2 || w[1].empty()) return "error: ui tip Press {button} to try it | [A, B, X, Y, LB or RB]\n";
        return test_tip(w[1].c_str(), w.size() > 2 ? w[2] : "")
                   ? "queued: it shows after 5 s of driving, at least 3 minutes after the last tip\n"
                   : "error: not queued; fh2-mods\\platform.log says why\n";
    }
    if (same_word(sub, "demo")) return demo();
    const int d = same_word(sub, "keyboard") ? 1 : same_word(sub, "pad") ? 0 : -1;
    popups_force_device(d);
    log_line(g_self.name, d < 0 ? "buttons in tips follow the device used last again" : std::string("buttons in tips are written for ") +
                                                                                            device_text(d) + " from now on, whatever was used last");
    return d < 0 ? "buttons in tips follow the device used last again\n" : "buttons in tips are written for " + device_text(d) + " now\n";
}

void on_command(void*, const char*, const char* args, char* reply, size_t size) {
    const std::string out = run_ui(args ? args : "");
    std::snprintf(reply, size, "%s", out.c_str());
}

// ---- the platform's own pages -------------------------------------------------------------------------------

const char* const kRecent = "Recent";
const char* const kTest   = "UiTemplates";

// Recent pop-ups' rows, as they are now: each time YOUR MODS opens (on_open), and once before it's first written.
void recent_rows() {
    const std::vector<RecentRow> rows = recent_page(popups_recent(), [](const char* english) { return tr(english); });
    std::vector<FH2PageRow> page(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        page[i].size        = sizeof(FH2PageRow);
        page[i].label       = rows[i].label.c_str();
        page[i].value       = rows[i].value.c_str();
        page[i].description = rows[i].description.c_str();
        page[i].style       = rows.size() == 1 && rows[i].value.empty() ? FH2_ROW_DIM : FH2_ROW_NORMAL;   // none yet
    }
    pages_set_rows(&g_self, kRecent, page.data(), static_cast<uint32_t>(page.size()));
}
void on_open_recent(void*, const char*) { recent_rows(); }

// The UI templates test page (Developer=1): A on a row fires that template once, from the platform. The design (5.4)
// also asks for a test language row; there's none, as for players: the test language is test=1 in
// fh2-mods\platform-language.ini, read once at start-up (l10n.cpp), since text set in fh2mod_init and the title
// screen's lines can't change language during a session, and a row would leave a mix.
struct TestRow {
    const char *key, *label, *description;
};
constexpr TestRow kTestRows[] = {
    {"Toast", "Pop-up", "A pop-up with the trophy picture. Pop-ups wait while you're in the menus, so it shows once you're driving."},
    {"Urgent", "Urgent pop-up", "A fault the player must act on: first in the queue, twice a session; later ones are normal pop-ups."},
    {"Reply", "Reply pop-up", "Answers this press: before the normal ones, and dropped if it can't show within 6 s of driving."},
    {"Private", "Private pop-up", "Its text may name people: the log and Recent pop-ups show only its key and \"(private)\"."},
    {"Tip", "Tip", "A tip with a button, after 5 s of driving: (A) on the controller, [SPACE] on the keyboard. Tips are 3 minutes apart, "
                   "three a session."},
    {"Sound", "Sound", "Plays the next of the game's UI sounds, Select first. The log says which, and whether it played."},
    {"Box", "Message box", "The game's message box, with two choices and Back. The log says what was chosen."},
    {"Hint", "Hint line", "The next of this page's hint texts: info, attention, fault, then none."},
    {"Keyboard", "Buttons: keyboard", "Tips write their buttons as keys from now on, whatever was used last."},
    {"Pad", "Buttons: controller", "Tips write their buttons for the controller from now on, whatever was used last."},
    {"Auto", "Buttons: as used last", "Tips write their buttons for the device used last, as they do for players."},
    {"Demo", "Everything in turn", "Four pop-ups and a tip, each saying what it is, and a sound."},
    {"Clear", "Take down the waiting ones", "Takes down the platform's test pop-ups that are still waiting."},
};
std::atomic<uint32_t> g_sound{0};
std::atomic<int> g_hint{-1};

void on_box_choice(void*, const char*, uint32_t choice) {
    log_line(g_self.name, "UI templates page: the message box was answered: " +
                              std::string(choice == 0 ? "Keep" : choice == 1 ? "Reset" : choice == FH2_BOX_BACK ? "Back" : "?"));
}

void on_select_test(void*, const char*, const char* row) {
    const std::string key = row ? row : "";
    if (key == "Toast") test_toast("Test pop-up", "From the UI templates page", FH2_ICON_TROPHY, 0);
    else if (key == "Urgent") test_toast("Test fault", "Do this to fix it", FH2_ICON_NONE, FH2_TOAST_FLAG_URGENT, "TestUrgent");
    else if (key == "Reply") test_toast("Test reply", "Answers your press", FH2_ICON_PHOTO, FH2_TOAST_FLAG_REPLY, "TestReply");
    else if (key == "Private") test_toast("Signed in as", "SomeGamertag", FH2_ICON_CLUB, FH2_TOAST_FLAG_PRIVATE, "TestPrivate");
    else if (key == "Tip") test_tip("Press {button} to try the test tip", "A");
    else if (key == "Sound") {
        const uint32_t s = g_sound++ % 11 + 1;
        const int played = g_api->play_ui_sound(&g_self, s);
        log_line(g_self.name, "UI templates page: sound " + std::to_string(s) + (played ? " played" : " didn't play (the line above may say why)"));
    } else if (key == "Box") {
        static const char* const choices[] = {"Keep", "Reset"};
        FH2MessageBox b{};
        b.size         = sizeof b;
        b.key          = "Test";
        b.title        = "Reset the test?";
        b.message      = "A test of the game's message box from the UI templates page. Nothing is reset either way.";
        b.choices      = choices;
        b.choice_count = 2;
        b.on_choose    = on_box_choice;
        if (!g_api->show_message_box(&g_self, &b)) log_line(g_self.name, "UI templates page: the message box didn't show (the line above may say why)");
    } else if (key == "Hint") {
        const int next = g_hint.load() >= 2 ? -1 : g_hint.load() + 1;
        g_hint         = next;
        const uint32_t kinds[] = {FH2_HINT_INFO, FH2_HINT_ATTENTION, FH2_HINT_FAULT};
        if (!g_api->show_page_hint(&g_self, kTest, next, next < 0 ? FH2_HINT_INFO : kinds[next]))
            log_line(g_self.name, "UI templates page: the hint didn't change (the line above may say why)");
    } else if (key == "Keyboard" || key == "Pad" || key == "Auto") {
        run_ui(key == "Keyboard" ? "keyboard" : key == "Pad" ? "pad" : "auto");
    } else if (key == "Demo") {
        std::string d = demo();
        if (!d.empty() && d.back() == '\n') d.pop_back();
        log_line(g_self.name, "UI templates page: " + d);
    } else if (key == "Clear") {
        g_api->clear_toast(&g_self, nullptr);
    }
}

} // namespace

// ---- start-up ---------------------------------------------------------------------------------------------------

FH2Mod* platform_self() { return &g_self; }
bool ui_developer() { return g_developer.load(); }

void ui_player_start(const FH2ModApi* api) {
    g_api       = api;
    g_self.name = "Platform";
    g_self.wname = L"Platform";
    const fs::path ini = ini_path();
    g_developer  = GetPrivateProfileIntW(L"popups", L"Developer", 0, ini.c_str()) == 1;
    g_mod_popups = GetPrivateProfileIntW(L"player", L"ModPopups", 1, ini.c_str()) != 0;
    g_tips       = GetPrivateProfileIntW(L"player", L"Tips", 1, ini.c_str()) != 0;
    read_mods(ini);
    popups_set_platform_toasts(g_mod_popups);
    popups_set_tips(g_tips);
    if (!g_mod_popups) log_line(kWho, "the player turned mod pop-ups off (Mod pop-ups, in fh2-mods\\platform-popups.ini)");
    if (!g_tips) log_line(kWho, "the player turned tips off (Tips, in fh2-mods\\platform-popups.ini)");
    if (g_developer)
        plog("Developer=1 (fh2-mods\\platform-popups.ini): the UI templates test page is on the FH2 Mod Platform's page under YOUR MODS, "
             "and \"fh2mod Platform ui\" has its test commands");
    command_add_platform(&g_self, "ui", "ui [recent]: what pop-ups are on screen and waiting, and why; Recent pop-ups", on_command, nullptr,
                         g_developer);
}

void ui_player_pages() {
    FH2Page p{};
    p.size        = sizeof p;
    p.key         = kRecent;
    p.title       = "Recent pop-ups";
    p.on_open     = on_open_recent;
    pages_add(&g_self, &p);
    recent_rows();
    if (!g_developer) return;
    FH2Page t{};
    t.size        = sizeof t;
    t.key         = kTest;
    t.title       = "UI templates";
    t.description = "Developer=1: each row shows one of the UI templates once, from the platform. The log says what happened.";
    t.on_select   = on_select_test;
    pages_add(&g_self, &t);
    std::vector<FH2PageRow> rows(sizeof kTestRows / sizeof kTestRows[0]);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].size        = sizeof(FH2PageRow);
        rows[i].key         = kTestRows[i].key;
        rows[i].label       = kTestRows[i].label;
        rows[i].description = kTestRows[i].description;
    }
    pages_set_rows(&g_self, kTest, rows.data(), static_cast<uint32_t>(rows.size()));
    static const char* const hints[] = {"A hint: info.", "A hint: attention.", "A hint: a fault."};
    g_api->set_page_hints(&g_self, kTest, hints, 3);   // as a mod does from fh2mod_init
}

// ---- the rows ---------------------------------------------------------------------------------------------------

PlayerText player_row_text(PlayerRow row) {
    PlayerText t;
    if (row == PlayerRow::mod_popups) {
        t.label       = tr("Mod pop-ups");
        t.description = tr("Same as Notifications: mods' pop-ups show while the game's Notifications option is on. Off: no mod shows "
                           "pop-ups. Each mod's page can turn its own off too.");
        t.choices     = {tr("Same as Notifications"), tr("Off")};
    } else {
        t.label       = tr("Tips");
        t.description = tr("Hints from mods, such as which button does what, each shown once. Off: no mod shows tips.");
    }
    return t;
}

int player_row_value(PlayerRow row) { return row == PlayerRow::mod_popups ? (g_mod_popups ? 0 : 1) : (g_tips ? 1 : 0); }

void player_row_changed(PlayerRow row, int value) {
    if (row == PlayerRow::mod_popups) {
        g_mod_popups = value == 0;
        popups_set_platform_toasts(g_mod_popups);
        log_line(kWho, std::string("the player set Mod pop-ups to ") + (g_mod_popups ? "Same as Notifications" : "Off"));
    } else {
        g_tips = value != 0;
        popups_set_tips(g_tips);
        log_line(kWho, std::string("the player turned tips ") + (g_tips ? "on" : "off"));
    }
    save();
}

bool mod_gets_popups_row(const FH2Mod* mod) {
    const uint32_t uses = mod ? ui_uses_of(mod) : 0;
    return (uses & FH2_UI_USES_TOASTS) && !(uses & FH2_UI_USES_OWN_SWITCH);
}

PlayerText mod_popups_text(const std::string& mod_name) {
    PlayerText t;
    t.label       = tr("Pop-ups");
    t.description = trf("Off: {0} shows no pop-ups. Mod pop-ups, on the FH2 Mod Platform's page under YOUR MODS, turns every mod's off.",
                        {mod_name});
    return t;
}

int mod_popups_start(FH2Mod* mod, const std::wstring& dll) {
    bool on = true;
    {
        std::scoped_lock lk{g_mu};
        if (const auto it = g_mod_on.find(lower(dll)); it != g_mod_on.end()) on = it->second;
    }
    popups_set_mod_toasts(mod, on);
    if (!on) log_line(mod->name, "its pop-ups are off: the player turned them off on its page (fh2-mods\\platform-popups.ini)");
    return on ? 1 : 0;
}

void mod_popups_changed(FH2Mod* mod, const std::wstring& dll, int value) {
    {
        std::scoped_lock lk{g_mu};
        g_mod_on[lower(dll)] = value != 0;
    }
    popups_set_mod_toasts(mod, value != 0);
    log_line(mod->name, std::string("the player turned its pop-ups ") + (value ? "on" : "off") + " on its page");
    save();
}

// ---- the platform's page ----------------------------------------------------------------------------------------

PlayerText platform_line() {
    PlayerText t;
    t.label       = "FH2 Mod Platform";
    t.description = tr("Pop-ups and tips from every mod, and the last ones shown.");
    t.hint        = tr("Press A to open it.");
    return t;
}

std::vector<PlatformLine> platform_page_lines() {
    std::vector<PlatformLine> lines;
    PlatformLine heading{PlatformLine::heading, "", {}};
    heading.text.label       = tr("Pop-ups and tips");
    heading.text.description = tr("Mods' pop-ups and tips show on the game's own notification, at the bottom of the screen while you drive.");
    lines.push_back(heading);
    lines.push_back({PlatformLine::mod_popups, "", player_row_text(PlayerRow::mod_popups)});
    lines.push_back({PlatformLine::tips, "", player_row_text(PlayerRow::tips)});
    PlatformLine again{PlatformLine::action, "TipsAgain", {}};
    again.text.label       = tr("Show tips again");
    again.text.description = tr("Every mod's tips show again, once each, as if they were new.");
    again.text.hint        = tr("Press A to show them again.");
    lines.push_back(again);
    lines.push_back({PlatformLine::page, kRecent, {}});
    if (g_developer) lines.push_back({PlatformLine::page, kTest, {}});
    return lines;
}

bool player_select(const std::string& key) {
    if (key != "TipsAgain") return false;
    log_line(kWho, "the player asked for every mod's tips again");
    popups_tips_again();
    return true;
}

bool platform_page_text(const std::string& key, std::string& title, std::string& label, std::string& description) {
    if (key == kRecent) {
        title = label = tr("Recent pop-ups");
        description   = tr("The last pop-ups and tips mods showed, newest first, with the mod that sent each.");
        return true;
    }
    if (key == kTest) {
        title = label = "UI templates";
        description   = "Developer=1: each row shows one of the UI templates once, from the platform. The log says what happened.";
        return true;
    }
    return false;
}

} // namespace fh2platform
