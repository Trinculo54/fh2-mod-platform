// A mod for the platform test: notices, conflicting edits, a late edit,
// file overrides, database changes, an interface for AaaMod, pages (API 8),
// title-screen entries (API 9), translations and the UI templates (API 10).
#include "fh2mod.h"
#include "fh2mod_l10n.h"
#include "fh2mod_ui.h"
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <string>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

// tests/l10n/pl.po and pt_BR.po, built in by tools/l10n/po2inc.py (tests/run_platform_test.sh checks it's up to date)
#include "l10n/test_mod_l10n.inc"

struct MathV1 {
    uint32_t size;
    int (*add)(int, int);
};
static const MathV1 g_math{sizeof(MathV1), [](int a, int b) { return a + b; }};

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{sizeof info, "TestMod", "1.0.0", nullptr, "9.9.9", 0};
    g_mod = api->register_mod(&info);
    // Translations (API 10): Polish and Brazilian Portuguese; the platform test has a Brazilian Portuguese file in
    // fh2-mods\lang\TestMod that's used instead of the second.
    FH2_ADD_TRANSLATIONS(api, g_mod, test_mod_translations);
    // L10N(notice-line): TestMod's notice on the title screen, in each language's own words (the spaces at the end are trimmed)
    api->set_notice(g_mod, FH2_TR(api, g_mod, "notice", "Test notice line\n\nsecond line  "));
    api->replace_string(g_mod, "AIPlayers.str", L"Nocomply76", L"Buddy1");
    api->set_string(g_mod, "LoadingMessages.str", 0x6875, L"from TestMod");

    const char x[] = "<xml/>";
    api->set_file(g_mod, "drivatarglobalcache\\DefaultCache.bin", x, sizeof x - 1); // AaaMod has it
    api->set_file(g_mod, "Stripped/StringTables/EN/Main.str", x, sizeof x - 1);   // refused
    api->set_file(g_mod, "UI/../../evil.txt", x, sizeof x - 1);                    // refused
    api->set_file(g_mod, "media/UI/keep.xml", x, sizeof x - 1);
    api->set_file(g_mod, "UI/handmade.xml", x, sizeof x - 1);
    api->set_file(g_mod, "UI/temp.xml", x, sizeof x - 1);
    api->add_sql(g_mod, "UPDATE AIPlayers SET XP = 12345 WHERE Id = 1; UPDATE AIPlayers SET XP = 777 WHERE Id = 2");
    api->publish_interface(g_mod, "TestMod.Math", 1, &g_math);

    // Pages: one on the Achievements tile, with a row of each kind; a tile that doesn't exist.
    FH2Page pg{sizeof pg};
    pg.key = "Stats"; pg.title = "Test stats"; pg.label = "Test stats & more"; pg.description = "What TestMod counted.";
    pg.tile = "achievements";
    pg.on_select = [](void*, const char*, const char*) {};
    api->add_page(g_mod, &pg);
    FH2PageRow rows[4] = {};
    for (FH2PageRow& r : rows) r.size = sizeof r;
    rows[0].label = "Heading row"; rows[0].value = "2 / 3"; rows[0].style = FH2_ROW_HEADING;
    rows[1].label = "Dim row"; rows[1].value = "Locked"; rows[1].style = FH2_ROW_DIM; rows[1].footer = "Footer <line> \xC2\xB7" " dot";   // UTF-8, written as &#183; (the game reads pages as 1252)
    rows[2].label = "Picked row"; rows[2].key = "pick"; rows[2].description = "Press A";
    rows[3].label = "Red row"; rows[3].value = "Alert"; rows[3].value_alert = 1;
    api->set_page_rows(g_mod, "Stats", rows, 4);
    // ... and on its tile, a page of its own design (Installed mods still shows the rows)
    api->set_page_xaml(g_mod, "Stats", "<Color x:Key=\"Test_Color\">#FF102030</Color>", "<StackPanel>no FirstItem</StackPanel>", nullptr);
    api->set_page_xaml(g_mod, "Stats", nullptr, "<Grid x:Name=\"FirstItem\"><Button></Grid>", nullptr);   // refused: unbalanced
    FH2PageRow odd[2] = {rows[0], rows[1]};
    odd[1].size = 8;
    api->set_page_rows(g_mod, "Stats", odd, 2);                                                             // refused: sizes differ
    api->set_page_xaml(g_mod, "Stats", "<Color x:Key=\"Test_Color\">#FF102030</Color>",
                       "<Grid x:Name=\"TestContent\"><Button x:Name=\"FirstItem\" Visibility=\"{Binding FH2Mod_Slot_2_Is_1, "
                       "Converter={StaticResource FH2Mod_ShowIf}}\" /></Grid>",
                       "<ContentControl x:Name=\"DefaultHelpPanel\" Style=\"{StaticResource A_Style_ContentControl_DefaultButtonBar}\" />");
    if (api->get_page_slot(g_mod, "Stats", 2) == 0 && api->set_page_slot(g_mod, "Stats", 2, 1) && api->get_page_slot(g_mod, "Stats", 2) == 1 &&
        !api->set_page_slot(g_mod, "Stats", 16, 1) && !api->focus_page_element(g_mod, "Stats", "FirstItem"))
        api->log(g_mod, "page slots ok");
    FH2Page other{sizeof other};
    other.key = "Other"; other.title = "Other"; other.tile = "Nope";
    api->add_page(g_mod, &other);
    api->add_page(g_mod, &other);                         // refused: already there
    api->set_page_rows(g_mod, "Missing", rows, 1);        // refused: no such page
    // text on the page's tile; not on a page without one
    if (api->set_page_tile_text(g_mod, "Stats", "12 / 34 G") && !api->set_page_tile_text(g_mod, "Other", "x") &&
        !api->set_page_tile_text(g_mod, "Missing", "x") && api->set_page_tile_colors(g_mod, "Stats", 0xFF3667DB, 0xFFFEFEFE) &&
        !api->set_page_tile_colors(g_mod, "Other", 0, 0))
        api->log(g_mod, "tile text ok");

    // Title-screen entries: Y by default, X asked for (a list), Y asked for again (taken: the next
    // free one, RB), LB, then none left; a repeated key, a bad one, a long Polish label cut to 32
    // characters (not bytes) and shown in capitals beyond a-z (platform 2.9.0), lists with too many
    // choices, an empty choice or no on_choose.
    FH2TitleEntry te{sizeof te};
    te.key = "Garage"; te.label = "Drive the test garage";
    te.on_press = [](void*, const char*) { return FH2_TITLE_START; };
    api->add_title_entry(g_mod, &te);
    api->add_title_entry(g_mod, &te);                     // refused: already there
    static std::string names[101];
    static const char* many[101];
    for (int i = 0; i < 101; ++i) {
        names[i] = "Track " + std::to_string(i + 1);
        many[i]  = names[i].c_str();
    }
    static const char* const blank[] = {"Fine", ""};
    static const char* const two[]   = {"Track one", "Track two"};
    te.key = "Second"; te.label = "Second entry"; te.button = FH2_BUTTON_X; te.on_press = nullptr;
    te.on_choose = [](void*, const char*, uint32_t choice) { return choice == 1 ? FH2_TITLE_START : FH2_TITLE_HANDLED; };
    te.choices = many; te.choice_count = 101;
    api->add_title_entry(g_mod, &te);                     // refused: 1 to 100
    te.choices = blank; te.choice_count = 2;
    api->add_title_entry(g_mod, &te);                     // refused: a choice without a label
    te.choices = two; te.on_choose = nullptr;
    api->add_title_entry(g_mod, &te);                     // refused: no on_choose
    te.on_choose = [](void*, const char*, uint32_t choice) { return choice == 1 ? FH2_TITLE_START : FH2_TITLE_HANDLED; };
    te.list_title = "Pick a track"; te.list_message = "Both are test tracks.";
    api->add_title_entry(g_mod, &te);
    te.choices = nullptr; te.choice_count = 0; te.on_choose = nullptr; te.list_title = nullptr; te.list_message = nullptr;
    te.on_press = [](void*, const char*) { return FH2_TITLE_START; };
    te.key = "Third"; te.label = "\xC5\xBB\xC3\xB3\xC5\x82ta trasa z bardzo d\xC5\x82ug\xC4\x85 nazw\xC4\x85 testow\xC4\x85"; te.button = FH2_BUTTON_Y;   // 40 characters, 47 bytes
    api->add_title_entry(g_mod, &te);
    te.key = "Fourth"; te.label = "Fourth & <last>"; te.button = 0;
    te.choices = many; te.choice_count = 20;              // a list in pages: 3 of 8
    te.on_choose = [](void*, const char*, uint32_t) { return FH2_TITLE_HANDLED; };
    api->add_title_entry(g_mod, &te);
    te.choices = nullptr; te.choice_count = 0; te.on_choose = nullptr;
    te.key = "Fifth"; te.label = "No room";
    api->add_title_entry(g_mod, &te);                     // refused: four at most
    te.key = "bad key";
    api->add_title_entry(g_mod, &te);                     // refused: not a key
    if (!api->title_started_by(g_mod)) api->log(g_mod, "no title start yet");

    // A choice row (API 9), its index through get_setting and set_setting (kept in range), and
    // two refused: one choice, and more than FH2_SETTING_MAX_CHOICES.
    static const char* const buttons[] = {"LB", "RB", "A & <B>"};
    static const char* const lots[FH2_SETTING_MAX_CHOICES + 1] = {"x", "x"};
    FH2Setting ch{sizeof ch};
    ch.key = "Button"; ch.label = "Test button"; ch.description = "Which button.";
    ch.kind = FH2_SETTING_CHOICE; ch.choices = buttons; ch.choice_count = 3; ch.value = 1;
    ch.on_change = [](void*, const char*, int) {};
    if (api->add_setting(g_mod, &ch) && api->get_setting(g_mod, "Button") == 1) {
        api->set_setting(g_mod, "Button", 7);
        if (api->get_setting(g_mod, "Button") == 2) api->log(g_mod, "choice row ok");
        api->set_setting(g_mod, "Button", 1);
    }
    ch.key = "One"; ch.choice_count = 1;
    api->add_setting(g_mod, &ch);                         // refused: one choice
    ch.key = "Lots"; ch.choices = lots; ch.choice_count = FH2_SETTING_MAX_CHOICES + 1;
    api->add_setting(g_mod, &ch);                         // refused: too many
    api->log(g_mod, api->playing_online() ? "playing online" : "playing offline");

    // Text in the game's language (API 10): no game here, so no language yet. The choice row's label
    // and its second choice renamed (the pages are written after this, so they show it), the
    // description left as it is; refused: the wrong number of choices, a row that isn't there.
    char lang[8];
    if (!api->game_language(lang, sizeof lang) && !lang[0]) api->log(g_mod, "no game language yet");
    static const char* const renamed[] = {"LB", "RB & <renamed>", "A & <B>"};
    if (api->set_setting_text(g_mod, "Button", "Test button, renamed", nullptr, renamed, 3)) api->log(g_mod, "setting text ok");
    api->set_setting_text(g_mod, "Button", nullptr, nullptr, renamed, 2);   // refused: it has 3
    api->set_setting_text(g_mod, "Nope", "x", nullptr, nullptr, 0);          // refused: no such row
    api->set_mod_description(g_mod, "TestMod's description, set later.");

    // Translations (API 10), in the language last time (Polish: the platform test writes platform-language.ini).
    api->log(g_mod, (std::string("language ") + api->language()).c_str());
    FH2Setting greet{sizeof greet};
    greet.key = "Greeting";
    // L10N: a row on the Mods page
    greet.label = FH2_TR(api, g_mod, "row.Greeting.label", "Test greeting");
    greet.description = FH2_TR(api, g_mod, "row.Greeting.description", "A row whose text is in the game's language.");
    greet.on_change = [](void*, const char*, int) {};
    api->add_setting(g_mod, &greet);
    api->log(g_mod, ("plural: " + fh2_fill(FH2_TR_PLURAL(api, g_mod, "count.cars", "{n} car", "{n} cars", 5), {{"n", fh2_number(api, 5)}})).c_str());
    api->log(g_mod, (std::string("stale: ") + FH2_TR(api, g_mod, "row.Stale.label", "New English")).c_str());
    api->log(g_mod, (std::string("fuzzy: ") + FH2_TR(api, g_mod, "row.Fuzzy.label", "Not sure yet")).c_str());
    char date[32];
    api->format_date(2026, 11, 8, date, sizeof date);
    api->log(g_mod, ("number: " + fh2_number(api, 12345.5, 1) + ", date: " + date).c_str());
    // each language's string table with its own text
    api->set_string_tr(g_mod, "LoadingMessages.str", 0x5507, "loading.tip", L"A loading tip from TestMod");

    // UI templates (API 10): all twelve members are there. ui_uses is real (an unknown bit is ignored, a second call
    // adds to the first). Pop-ups and tips queue with no game (their hooks wait for it): a pop-up taken down while
    // waiting, a private one, a bad key refused; a tip queued, marked seen (which drops it), then refused as seen,
    // and one whose text has no {button}. The others are being built, so each returns 0 and says so in the log
    // once. Then the same through fh2mod_ui.h.
    {
        const bool all = api->size >= offsetof(FH2ModApi, show_page_hint) + sizeof api->show_page_hint && api->ui_status &&
                         api->ui_uses && api->play_ui_sound && api->show_toast && api->clear_toast && api->show_tip &&
                         api->tip_seen && api->set_tip_seen && api->show_message_box && api->set_page_help &&
                         api->set_page_hints && api->show_page_hint;
        api->log(g_mod, all ? "ui templates: all 12 members there" : "ui templates: a member is missing");
        FH2Toast toast{sizeof toast};
        toast.key = "Test"; toast.title = "Test pop-up"; toast.subtitle = "From TestMod"; toast.icon = FH2_ICON_TROPHY;
        FH2Tip tip{sizeof tip};
        tip.key = "FirstTip"; tip.text = "Press {button} to test"; tip.button.glyph = FH2_GLYPH_A; tip.button.vk = VK_SPACE;
        static const char* const box_choices[] = {"Keep", "Reset"};
        FH2MessageBox box{sizeof box};
        box.key = "Reset"; box.title = "Reset?"; box.choices = box_choices; box.choice_count = 2;
        FH2Prompt help{};
        help.glyph = FH2_GLYPH_A; help.word = FH2_WORD_SELECT;
        static const char* const hints[] = {"Saved."};
        const int uses        = api->ui_uses(g_mod, FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS | 0x100);
        const uint32_t status = api->ui_status(g_mod);
        const int toasted     = api->show_toast(g_mod, &toast);
        api->clear_toast(g_mod, "Test");
        FH2Toast secret{sizeof secret};
        secret.key = "SignIn"; secret.title = "Signed in as"; secret.subtitle = "TestGamertag"; secret.flags = FH2_TOAST_FLAG_PRIVATE;
        FH2Toast bad{sizeof bad};
        bad.key = "no spaces"; bad.title = "Bad key";
        const int toasts = api->show_toast(g_mod, &secret) + api->show_toast(g_mod, &bad);
        const int tipped = api->show_tip(g_mod, &tip);
        const int seen0  = api->tip_seen(g_mod, "FirstTip");
        api->set_tip_seen(g_mod, "FirstTip", 1);
        const int seen1 = api->tip_seen(g_mod, "FirstTip");
        FH2Tip no_slot{sizeof no_slot};
        no_slot.key = "NoSlot"; no_slot.text = "Press the button"; no_slot.button.glyph = FH2_GLYPH_X;
        const int tips_again = api->show_tip(g_mod, &tip) + api->show_tip(g_mod, &no_slot);
        const int others = api->play_ui_sound(g_mod, FH2_SOUND_SELECT) + api->show_message_box(g_mod, &box) +
                           api->set_page_help(g_mod, "Stats", &help, 1) + api->set_page_hints(g_mod, "Stats", hints, 1) +
                           api->show_page_hint(g_mod, "Stats", 0, FH2_HINT_INFO);
        api->show_toast(g_mod, &toast);   // queued again
        api->log(g_mod, ("ui templates: ui_uses " + std::to_string(uses) + ", ui_status " + std::to_string(status) + ", show_toast " +
                         std::to_string(toasted) + " then " + std::to_string(toasts) + ", show_tip " + std::to_string(tipped) + ", tip_seen " +
                         std::to_string(seen0) + " then " + std::to_string(seen1) + ", show_tip again " + std::to_string(tips_again) +
                         ", the others " + std::to_string(others))
                            .c_str());
        api->ui_uses(g_mod, FH2_UI_USES_OWN_SWITCH);
        fh2_helpers_init(api, g_mod);
        if (fh2ui_ui_status() == 0 && fh2ui_tip_seen("FirstTip") == 1 && fh2ui_notify_or_log("Test pop-up", "from the helper") == 1)
            api->log(g_mod, "ui helpers ok");
    }

    // Sessions: the id, and a log of our own named after it (three kept); a bad folder name.
    api->log(g_mod, (std::string("session id ") + api->session_id()).c_str());
    if (const wchar_t* p = api->session_log_path(g_mod, L"TestModData", 3)) {
        if (FILE* f = _wfopen(p, L"wb")) {
            std::fputs("test mod's own log\n", f);
            std::fclose(f);
        }
    }
    if (!api->session_log_path(g_mod, L"..\\Elsewhere", 3)) api->log(g_mod, "bad log folder refused");
    // Hotkeys: F9 and LB+RB+Y; a key of its own (Ctrl+Shift+M, offered as a choice besides the
    // F-keys); a repeated key; no on_press.
    FH2Hotkey hk{sizeof hk};
    hk.key = "Mark"; hk.label = "Mark this spot"; hk.vk = 0x78; /* VK_F9 */ hk.pad_chord = FH2_PAD_LB | FH2_PAD_RB | FH2_PAD_Y;
    hk.on_press = [](void*, const char*) {};
    hk.on_rebind = [](void*, const char*, uint32_t, uint32_t, uint32_t) {};
    api->add_hotkey(g_mod, &hk);
    api->add_hotkey(g_mod, &hk);                          // refused: already there
    hk.key = "Toggle"; hk.label = "Toggle the test"; hk.vk = 'M'; hk.modifiers = FH2_KEY_CTRL | FH2_KEY_SHIFT; hk.pad_chord = 0;
    api->add_hotkey(g_mod, &hk);
    hk.key = "Broken"; hk.on_press = nullptr;
    api->add_hotkey(g_mod, &hk);                          // refused: no on_press
    // Commands: echo (answers with its arguments), fail (an error), a bad name, a repeat.
    // echo also answers with a pop-up: a reply, since the platform has just called the command (API 10)
    api->add_command(g_mod, "echo", "echo <text>: answers with the text",
                     [](void*, const char*, const char* args, char* reply, size_t size) {
                         std::snprintf(reply, size, "%s", args);
                         FH2Toast t{sizeof t};
                         t.key = "Echo"; t.title = "Echo"; t.subtitle = args; t.flags = FH2_TOAST_FLAG_REPLY;
                         g_api->show_toast(g_mod, &t);
                     },
                     nullptr);
    api->add_command(g_mod, "fail", nullptr,
                     [](void*, const char*, const char*, char* reply, size_t size) { std::snprintf(reply, size, "error: as asked"); },
                     nullptr);
    api->add_command(g_mod, "bad name", nullptr, [](void*, const char*, const char*, char*, size_t) {}, nullptr);   // refused
    api->add_command(g_mod, "Echo", nullptr, [](void*, const char*, const char*, char*, size_t) {}, nullptr);       // refused: echo
    // A copy of the saves, if the test made a saves folder (the full phase does).
    if (api->request_save_snapshot(g_mod, "the platform test", 2)) api->log(g_mod, "save snapshot asked for");

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(300);
        g_api->replace_string(g_mod, "AIPlayers.str", L"Super Final", L"Buddy2");
        g_api->apply_strings(g_mod);
        g_api->clear_file(g_mod, "UI/temp.xml");
        g_api->ui_uses(g_mod, FH2_UI_USES_TOASTS);   // refused: after fh2mod_init
        g_api->log(g_mod, "late edit applied");
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}
