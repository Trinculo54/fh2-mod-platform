// A mod for the platform test: notices, conflicting edits, a late edit,
// file overrides, database changes, an interface for AaaMod, pages (API 8),
// title-screen entries (API 9).
#include "fh2mod.h"
#include <windows.h>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

struct MathV1 {
    uint32_t size;
    int (*add)(int, int);
};
static const MathV1 g_math{sizeof(MathV1), [](int a, int b) { return a + b; }};

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{sizeof info, "TestMod", "1.0.0", nullptr, "9.9.9", 0};
    g_mod = api->register_mod(&info);
    api->set_notice(g_mod, "Test notice line\n\nsecond line  ");
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
    rows[1].label = "Dim row"; rows[1].value = "Locked"; rows[1].style = FH2_ROW_DIM; rows[1].footer = "Footer <line>";
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

    // Title-screen entries: Y by default, X asked for, Y asked for again (taken: the next free one,
    // RB), LB, then none left; a repeated key, a bad one, a long label cut to 32 bytes.
    FH2TitleEntry te{sizeof te};
    te.key = "Garage"; te.label = "Drive the test garage";
    te.on_press = [](void*, const char*) { return FH2_TITLE_START; };
    api->add_title_entry(g_mod, &te);
    api->add_title_entry(g_mod, &te);                     // refused: already there
    te.key = "Second"; te.label = "Second entry"; te.button = FH2_BUTTON_X;
    api->add_title_entry(g_mod, &te);
    te.key = "Third"; te.label = "Third entry with a label that is far too long"; te.button = FH2_BUTTON_Y;
    api->add_title_entry(g_mod, &te);
    te.key = "Fourth"; te.label = "Fourth & <last>"; te.button = 0;
    api->add_title_entry(g_mod, &te);
    te.key = "Fifth"; te.label = "No room";
    api->add_title_entry(g_mod, &te);                     // refused: four at most
    te.key = "bad key";
    api->add_title_entry(g_mod, &te);                     // refused: not a key
    if (!api->title_started_by(g_mod)) api->log(g_mod, "no title start yet");

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(300);
        g_api->replace_string(g_mod, "AIPlayers.str", L"Super Final", L"Buddy2");
        g_api->apply_strings(g_mod);
        g_api->clear_file(g_mod, "UI/temp.xml");
        g_api->log(g_mod, "late edit applied");
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}
