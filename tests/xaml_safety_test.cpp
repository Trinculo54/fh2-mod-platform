// File and XAML safety (src/xaml_safety.cpp), with no game: the platform's own UI files in every spelling set_file
// could be given, the keys of a small fake UI.zip (written here, read back through strings.cpp's zip reading), image
// paths against small fake texture zips, the page check's rules, and the page crash guard's state. Runs in the
// current folder (it writes fake-media\ there). Exits 0 when every check passes.
//
// With FH2_MEDIA set to an install's media folder, the same check also runs against the real UI.zip and texture
// zips; any further arguments are XAML files (a page as the platform wrote it, say) that must pass it whole.
#include "strings.hpp"
#include "xaml_safety.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fh2platform;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

// ---- a zip of stored files ----

uint32_t crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
void put16(std::string& o, uint32_t v) { o += static_cast<char>(v & 0xFF), o += static_cast<char>((v >> 8) & 0xFF); }
void put32(std::string& o, uint32_t v) { put16(o, v & 0xFFFF), put16(o, v >> 16); }

void write_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, cd;
    for (const auto& [name, data] : files) {
        const auto at = static_cast<uint32_t>(out.size());
        const uint32_t crc = crc32(data), n = static_cast<uint32_t>(data.size());
        put32(out, 0x04034b50), put16(out, 20), put16(out, 0), put16(out, 0), put32(out, 0), put32(out, crc), put32(out, n), put32(out, n);
        put16(out, static_cast<uint32_t>(name.size())), put16(out, 0);
        out += name + data;
        put32(cd, 0x02014b50), put16(cd, 20), put16(cd, 20), put16(cd, 0), put16(cd, 0), put32(cd, 0), put32(cd, crc), put32(cd, n), put32(cd, n);
        put16(cd, static_cast<uint32_t>(name.size())), put16(cd, 0), put16(cd, 0), put16(cd, 0), put16(cd, 0), put32(cd, 0), put32(cd, at);
        cd += name;
    }
    const auto cd_at = static_cast<uint32_t>(out.size());
    out += cd;
    put32(out, 0x06054b50), put16(out, 0), put16(out, 0), put16(out, static_cast<uint32_t>(files.size())), put16(out, static_cast<uint32_t>(files.size()));
    put32(out, static_cast<uint32_t>(cd.size())), put32(out, cd_at), put16(out, 0);
    fs::create_directories(path.parent_path());
    std::ofstream{path, std::ios::binary} << out;
}

std::string lower_slash(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    return s;
}

// What page_guard.cpp does with the game folder: each texture zip's directory, read once.
struct Media {
    fs::path root;   // the media folder
    std::vector<char> ui;
    std::map<std::string, std::pair<bool, std::set<std::string>>> zips;
    int reads = 0;   // texture zips read
    int texture(const TextureSpot& s) {
        auto it = zips.find(lower_slash(s.zip));
        if (it == zips.end()) {
            std::vector<std::string> names;
            const bool ok = zip_names(root / "ui" / "textures" / s.zip, names);
            std::set<std::string> set;
            for (const auto& n : names) set.insert(lower_slash(n));
            it = zips.emplace(lower_slash(s.zip), std::pair{ok, set}).first;
            ++reads;
        }
        if (!it->second.first) return -1;
        return it->second.second.count(lower_slash(s.entry)) ? 1 : 0;
    }
    std::string read(const std::string& name) const {
        const std::vector<char> d = unzip_from(ui, name);
        return {d.begin(), d.end()};
    }
};

bool load_media(Media& m, AppKeys& app, std::string& why) {
    std::ifstream in{m.root / "UI.zip", std::ios::binary};
    m.ui.assign(std::istreambuf_iterator<char>(in), {});
    return load_app_keys([&](const std::string& n) { return m.read(n); }, app, why);
}

std::string slurp(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), {}};
}

// ---- the parts of the test ----

void platform_files() {
    const char* const theirs[] = {
        "UI/Scenes/Anthem/A_PauseMenu.xaml",       "media\\ui\\scenes\\anthem\\a_pausemenu.xaml", "UI.zip/Scenes/Anthem/A_Hud.xaml",
        "Scenes/Anthem/A_Splash.xaml",             "UI/Scenes/Anthem/A_ModPage_80.xaml",          "UI/Scenes/Anthem/A_ModPage_01.xaml.",
        "UI/Scenes/Anthem/A_ModOptions.xaml ",     "stateflow/pause.xml",                         "STATEFLOW.ZIP\\ui_screens.xml",
        "/UI/Scenes/Anthem/A_PauseOptions.xaml",   "Media/UI//Scenes/Anthem/A_HUD.XAML",          "UI\\Scenes\\Anthem\\A_ModPage_123.xaml",
    };
    bool all = true;
    for (const char* p : theirs) {
        const bool found = platform_ui_file(p) != nullptr;
        if (!found) std::printf("     not seen as the platform's: %s\n", p);
        all = all && found;
    }
    check(all, "every spelling of the platform's UI files is seen as the platform's");
    const char* const free[] = {"UI/Scenes/Anthem/A_HUDOptions.xaml",  "UI/Scenes/Anthem/A_ModPage_.xaml",  "UI/Scenes/Anthem/A_ModPage_ab.xaml",
                                "UI/Controls/HUDSpeedometerControl.xaml", "stateflow/other.xml",         "pause.xml",
                                "UI/Scenes/A_Hud.xaml",                 "UI/Scenes/Anthem/A_Hud.xaml.bak", "UI/Scenes/Anthem/Sub/A_Hud.xaml",
                                "drivatarglobalcache/DefaultCache.bin", "",                               "stateflow/pause.xml/more"};
    bool none = true;
    for (const char* p : free) {
        const bool found = platform_ui_file(p) != nullptr;
        if (found) std::printf("     seen as the platform's, but isn't: %s\n", p);
        none = none && !found;
    }
    check(none, "other files, and near misses, aren't the platform's");
    const PlatformUiFile* f = platform_ui_file("ui/scenes/anthem/a_pausemenu.xaml");
    check(f && std::string(f->path) == "UI\\Scenes\\Anthem\\A_PauseMenu.xaml" && std::string(f->what) == "the pause menu's MODS tab",
          "the entry says which file it is and what the platform uses it for");
    check(platform_ui_file("UI/Scenes/Anthem/A_ModPage_07.xaml") == &kPlatformUiFiles[5] && platform_ui_file("stateflow/pause.xml") == &kPlatformUiFiles[6],
          "A_ModPage_NN and stateflow\\pause.xml find their own entries");
    check(kPlatformUiFileCount == 8, "eight files are the platform's");
}

const char* const kApp =
    "<Application x:Class=\"ForzaUI.App\">\r\n"
    "    <Application.Resources>\r\n"
    "        <ResourceDictionary>\r\n"
    "            <ResourceDictionary.MergedDictionaries>\r\n"
    "                <ResourceDictionary Source=\"Resources/Anthem/Global_Colors.xaml\"/>\r\n"
    "                <!-- <ResourceDictionary Source=\"Resources/Anthem/Commented_Out.xaml\"/> -->\r\n"
    "                <ResourceDictionary Source=\"Resources/Anthem/Shared_Bits.xaml\" />\r\n"
    "            </ResourceDictionary.MergedDictionaries>\r\n"
    "            <Style x:Key=\"A_Style_Page_Generic\" TargetType=\"{x:Type Page}\" />\r\n"
    "        </ResourceDictionary>\r\n"
    "    </Application.Resources>\r\n"
    "</Application>\r\n";
const char* const kColors =
    "<ResourceDictionary xmlns=\"x\">\r\n"
    "    <Color x:Key=\"A_Color_White\">#FFFEFEFE</Color>\r\n"
    "    <Style x:Key=\"A_Style_FZTextBlock_Heading\" TargetType=\"local:FZTextBlock\">\r\n"
    "        <Style.Resources><Color x:Key=\"Nested_Key\">#FF000000</Color></Style.Resources>\r\n"
    "    </Style>\r\n"
    "</ResourceDictionary>\r\n";
const char* const kBits =
    "<ResourceDictionary>\r\n"
    "    <ControlTemplate x:Key=\"A_ControlTemplate_Plain\" TargetType=\"{x:Type Button}\"><Grid /></ControlTemplate>\r\n"
    "    <local:FZBooleanToVisibilityConverter x:Key=\"A_FZBooleanToVisibilityConverter\" />\r\n"
    "</ResourceDictionary>\r\n";

void fake_media(const fs::path& root) {
    write_zip(root / "UI.zip", {{"AnthemApp.xaml", kApp},
                                {"Resources/Anthem/Global_Colors.xaml", kColors},
                                {"Resources/Anthem/Shared_Bits.xaml", kBits},
                                {"Resources/Anthem/Common_Buttons.xaml", "<ResourceDictionary><Color x:Key=\"Common_Only\">#FF000000</Color></ResourceDictionary>"},
                                {"Scenes/Anthem/A_HUDOptions.xaml", "<Page />"}});
    write_zip(root / "ui" / "textures" / "Anthem.zip", {{"Icons\\Trophy_Icon_Medium.swatchbin", "burG"}, {"Icons/PauseMenu/Lock_Large.swatchbin", "burG"}});
    write_zip(root / "ui" / "textures" / "data_bound" / "ControllerIcons.zip", {{"Medium/A.swatchbin", "burG"}});
    write_zip(root / "ui" / "textures" / "Data_Bound.zip", {{"Logo_FM.swatchbin", "burG"}});
}

void zips_and_keys(Media& m, AppKeys& app) {
    std::vector<std::string> names;
    check(zip_names(m.root / "UI.zip", names) && names.size() == 5 && names[0] == "AnthemApp.xaml" && names[4] == "Scenes/Anthem/A_HUDOptions.xaml",
          "zip_names lists a zip's entries, as spelt there");
    check(!zip_names(m.root / "nothing.zip", names) && names.empty(), "zip_names says so when the zip isn't there");
    std::ofstream{m.root / "short.zip", std::ios::binary} << "PK";
    check(!zip_names(m.root / "short.zip", names), "zip_names says so when the file isn't a zip");
    std::string why;
    check(load_media(m, app, why), "the fake UI.zip's application keys are read");
    check(app.dictionaries == 2 && app.keys.size() == 5, "two merged dictionaries (not the one in a comment), five keys");
    check(app.keys.count("A_Style_Page_Generic") && app.keys.count("A_Color_White") && app.keys.count("A_FZBooleanToVisibilityConverter"),
          "AnthemApp.xaml's own key and its dictionaries' keys are there");
    check(!app.keys.count("Nested_Key") && !app.keys.count("Common_Only"), "a Style's own resources and a dictionary nobody merges aren't");
    AppKeys none;
    check(!load_app_keys([](const std::string&) { return std::string(); }, none, why) && why == "AnthemApp.xaml couldn't be read from UI.zip",
          "no UI.zip: the keys can't be read, and why says so");
    const std::string broken = std::string(kApp) + "<Extra>";
    check(!load_app_keys([&](const std::string& n) { return n == "AnthemApp.xaml" ? broken : m.read(n); }, none, why) &&
              why.rfind("AnthemApp.xaml's tags: ", 0) == 0,
          "AnthemApp.xaml that doesn't balance: refused with why");
    check(!load_app_keys([&](const std::string& n) { return n == "Resources/Anthem/Shared_Bits.xaml" ? std::string() : m.read(n); }, none, why) &&
              why == "Resources/Anthem/Shared_Bits.xaml couldn't be read from UI.zip",
          "a merged dictionary that can't be read: refused, naming it");
}

void spots() {
    auto one = [](const char* p, const char* zip, const char* entry) {
        const auto s = texture_spots(p);
        return s.size() == 1 && s[0].zip == zip && s[0].entry == entry;
    };
    check(one("/ForzaUI;component/Textures/Anthem/Icons/Trophy_Icon_Medium.png", "Anthem.zip", "Icons/Trophy_Icon_Medium.swatchbin"),
          "/ForzaUI;component/Textures/Anthem/... is Anthem.zip's .swatchbin");
    check(one("/Textures/Anthem/Icons/CRIcon\\EN\\Credit_Icon_1.png", "Anthem.zip", "Icons/CRIcon/EN/Credit_Icon_1.swatchbin"),
          "/Textures/... without /ForzaUI;component, and back slashes, as the game's own XAML has them");
    check(one("/ForzaUI;component/Textures\\Anthem\\HUD\\X.PNG", "Anthem.zip", "HUD/X.swatchbin") && one(" /Textures/WPF/bg.png ", "WPF.zip", "bg.swatchbin"),
          "any case for .png and the folder; WPF.zip; spaces around the value");
    const auto db = texture_spots("/ForzaUI;component/Textures/Data_Bound/ControllerIcons/Medium/A.png");
    check(db.size() == 2 && db[0].zip == "data_bound\\ControllerIcons.zip" && db[0].entry == "Medium/A.swatchbin" && db[1].zip == "Data_Bound.zip" &&
              db[1].entry == "ControllerIcons/Medium/A.swatchbin",
          "Data_Bound: its folder's own zip first, then Data_Bound.zip");
    bool none = true;
    for (const char* p : {"Images/x.png", "/Textures/Anthem/x.jpg", "pack://siteoforigin:,,,/Textures/Anthem/x.png", "/Textures/Backgrounds/x.png",
                          "/Textures/Anthem/../x.png", "/Textures/Anthem//x.png", "/Textures/x.png", "C:\\Users\\me\\x.png", "/Textures/Anthem/.png"})
        none = none && texture_spots(p).empty();
    check(none, "anything else isn't one of the game's texture paths (other forms, folders, .., empty parts)");
}

void fragments(Media& m, const AppKeys& app) {
    const TextureLookup tex = [&](const TextureSpot& s) { return m.texture(s); };
    FragmentCheck c;
    std::string why;
    const std::string res =
        "<Style x:Key=\"Mod_Text\" TargetType=\"local:FZTextBlock\" BasedOn=\"{StaticResource A_Style_FZTextBlock_Heading}\">\r\n"
        "    <Setter Property=\"Color\" Value=\"{StaticResource A_Color_White}\" />\r\n"
        "</Style>\r\n"
        "<Style x:Key=\"Mod_Icon\" TargetType=\"local:FZMaterial\">\r\n"
        "    <Setter Property=\"ImageSource\" Value=\"/ForzaUI;component/Textures/Anthem/Icons/PauseMenu/Lock_Large.png\" />\r\n"
        "    <Style.Resources><Color x:Key=\"Mod_Nested\">#FF000000</Color></Style.Resources>\r\n"
        "</Style>";
    const std::string content =
        "<Grid>\r\n"
        "  <!-- {StaticResource In_A_Comment} -->\r\n"
        "  <Button x:Name=\"FirstItem\" Template=\"{StaticResource A_ControlTemplate_Plain}\" Style=\"{StaticResource {x:Type Button}}\">\r\n"
        "    <local:FZTextBlock Text=\"{}{StaticResource Not_A_Key}\" Style=\"{StaticResource ResourceKey=Mod_Text}\" />\r\n"
        "    <local:FZTextBlock Text=\"photo.png and /slashes/\" Color=\"{DynamicResource Mod_Nested}\" />\r\n"
        "    <local:FZMaterial ImageSource=\"/ForzaUI;component/Textures/Anthem/Icons/Trophy_Icon_Medium.png\" Visibility=\"{Binding FH2Mod_Slot_1_Is_1, Converter={StaticResource FH2Mod_ShowIf}}\" />\r\n"
        "    <Image Source=\"/Textures/Data_Bound/ControllerIcons/Medium/A.png\" Tag=\"{Binding X}\" />\r\n"
        "    <local:FZMaterial Icon=\"/Textures/Data_Bound/Logo_FM.png\" />\r\n"
        "    <ResourceDictionary Source=\"Resources/Anthem/Shared_Bits.xaml\" />\r\n"
        "  </Button>\r\n"
        "</Grid>";
    const std::string help = "<ContentControl x:Name=\"DefaultHelpPanel\" Visibility=\"{Binding A, Converter={StaticResource FH2Mod_HideIf}}\" />";
    check(check_page_fragment(app, res, content, help, tex, c, why), "a fragment that only uses what's there passes");
    if (!why.empty()) std::printf("     (%s)\n", why.c_str());
    check(c.own_keys == 3 && c.keys_used == 7, "its own keys (a Style's nested one included) and the distinct keys it uses are counted");
    check(c.images == 4 && c.images_unchecked == 0, "four images, all found: Anthem.zip, a Data_Bound folder's zip and Data_Bound.zip");
    check(check_page_fragment(app, "", "<Button x:Name=\"FirstItem\" />", "", tex, c, why) && c.keys_used == 0 && c.images == 0,
          "a fragment with no keys or images passes");

    auto refused = [&](const std::string& r, const std::string& cn, const std::string& h, const std::string& want) {
        std::string w;
        FragmentCheck cc;
        const bool ok = !check_page_fragment(app, r, cn, h, tex, cc, w) && w.find(want) != std::string::npos;
        if (!ok) std::printf("     why: %s\n", w.c_str());
        return ok;
    };
    check(refused("", "<Grid Style=\"{StaticResource Mod_Missing}\" />", "",
                  "the resource key Mod_Missing, in the content, isn't one of the game's shared resources, the page's own or the fragment's"),
          "a key that isn't anywhere: refused, naming the key and the part");
    check(refused("", "", "<Grid Visibility=\"{Binding A, Converter={StaticResource Missing_Converter}}\" />", "the resource key Missing_Converter, in the help,"),
          "a key inside a binding, in the help: refused");
    check(refused("<Style x:Key=\"S\" BasedOn=\"{StaticResource A_Style_FZTextBlock_TileClubMemberCount}\" />", "", "",
                  "the resource key A_Style_FZTextBlock_TileClubMemberCount, in the resources,"),
          "a key only a scene of the game's own has (A_PauseMenu's): refused");
    check(refused("", "<Grid><Grid.Background><StaticResource ResourceKey=\"Element_Missing\" /></Grid.Background></Grid>", "",
                  "the resource key Element_Missing, in the content,"),
          "<StaticResource ResourceKey=\"...\"/> is checked too");
    check(refused("", "<Grid Background=\"{DynamicResource Common_Only}\" />", "", "Common_Only"),
          "a key of a dictionary no page merges (Common_*): refused");
    check(refused("", "<Grid Background=\"{StaticResource}\" />", "", "a {StaticResource} or {DynamicResource} in the content has no key"),
          "{StaticResource} without a key: refused");
    check(refused("", "<Grid Background=\"{StaticResource Nested_Key}\" />", "", "Nested_Key"),
          "a key from inside a game Style's own resources: refused");
    check(refused("", "<Image Source=\"/ForzaUI;component/Textures/Anthem/Icons/Nope.png\" />", "",
                  "the image \"/ForzaUI;component/Textures/Anthem/Icons/Nope.png\", in the content, isn't one of the game's textures "
                  "(media\\ui\\textures\\Anthem.zip has no Icons/Nope.swatchbin)"),
          "an image that isn't in its zip: refused, naming the path, the zip and the entry");
    check(refused("<Setter Property=\"ImageSource\" Value=\"Images/mine.png\" />", "", "",
                  "the image \"Images/mine.png\", in the resources, isn't one of the game's texture paths"),
          "an image path in another form: refused");
    check(refused("", "<Grid Tag=\"/Textures/Anthem/Icons/Nope.png\" />", "", "Icons/Nope.swatchbin"),
          "a texture path in any attribute is checked (the game's XAML has them in Tag)");
    check(refused("", "<Image Source=\"pack://siteoforigin:,,,/Textures/Anthem/Icons/Trophy_Icon_Medium.png\" />", "", "isn't one of the game's texture paths"),
          "pack:// paths (Forza Motorsport 5's, never loaded) are refused");
    check(refused("<ImageSource x:Key=\"Mod_Glyph\">/ForzaUI;component/Textures/Anthem/Icons/Gone.png</ImageSource>", "", "",
                  "the image \"/ForzaUI;component/Textures/Anthem/Icons/Gone.png\", in the resources,"),
          "a path as an element's text (<ImageSource x:Key=...>path</ImageSource>) is checked too");
    check(refused("", "<Image Source=\"/Textures/Data_Bound/Nowhere/x.png\" />", "", "data_bound\\Nowhere.zip has no x.swatchbin"),
          "a Data_Bound folder without a zip, and not in Data_Bound.zip either: refused");
    check(refused("", "<Grid><Button></Grid>", "", "the tags in the content don't balance: </Grid> closes <Button>"), "tags that don't balance: refused");

    // a zip that can't be read: the image is let through, and counted as unchecked
    Media gone{m.root / "nowhere"};
    check(check_page_fragment(app, "", "<Image Source=\"/Textures/Anthem/Icons/Anything.png\" />", "",
                              [&](const TextureSpot& s) { return gone.texture(s); }, c, why) &&
              c.images == 1 && c.images_unchecked == 1,
          "an image whose zip can't be read is let through, as unchecked");
    const int reads = m.reads;
    check_page_fragment(app, res, content, help, tex, c, why);
    check(m.reads == reads, "each texture zip's directory is read once");
}

void guard() {
    const std::string text =
        "# a comment\r\n"
        "opening\tTestMod.dll\tStats\t100-01D9\t2026-11-08\r\n"
        "skip\tOther.dll\tList\t200-02\t2026-11-01\n"
        "skip\tChanged.dll\tList\t300-03\t2026-11-01\r\n"
        "opening\tGone.dll\tPage\t400-04\t2026-11-02\r\n"
        "nonsense line\r\n"
        "skip\t\tNoDll\t1-1\t\r\n"
        "maybe\tX.dll\tP\t1-1\t\r\n"
        "skip\tother.DLL\tList\t200-02\t2026-11-03\r\n";
    const std::vector<GuardEntry> e = parse_guard(text);
    check(e.size() == 4 && e[0].state == "opening" && e[0].dll == "TestMod.dll" && e[0].page == "Stats" && e[0].stamp == "100-01D9" &&
              e[0].date == "2026-11-08",
          "the guard's file is read: four usable lines, the others left out");
    check(e[1].dll == "other.DLL" && e[1].date == "2026-11-03", "the same DLL (in any case) and page twice: the last line counts");
    const std::map<std::string, std::string> now = {{"TestMod.dll", "100-01D9"}, {"other.dll", "200-02"}, {"Changed.dll", "300-99"}};
    std::vector<std::string> lines;
    const std::vector<GuardEntry> after = guard_at_start(
        e,
        [&](const std::string& dll) {
            for (const auto& [k, v] : now)
                if (lower_slash(k) == lower_slash(dll)) return v;
            return std::string();
        },
        lines);
    check(after.size() == 2 && after[0].state == "skip" && after[0].dll == "TestMod.dll" && after[0].date == "2026-11-08" && after[1].state == "skip" &&
              after[1].dll == "other.DLL",
          "a page still opening is left out from now on, and a left-out one stays so while its DLL is the same");
    auto said = [&](const std::string& part) {
        for (const auto& l : lines)
            if (l.find(part) != std::string::npos) return true;
        return false;
    };
    check(said("the game stopped while TestMod.dll's page Stats was opening last session, which most likely means it crashed loading it; "
               "the page is left out until TestMod.dll changes (an update or a reinstall)"),
          "the log says which page is left out, and until when");
    check(said("other.DLL's page List is still left out: the game stopped while it was opening on 2026-11-03, and other.DLL hasn't changed since"),
          "a page still left out says since when");
    check(said("Changed.dll has changed since Changed.dll's page List was left out, so the page gets another try") &&
              said("Gone.dll isn't in fh2-mods any more, so the note about Gone.dll's page Page is dropped"),
          "a changed DLL's page gets another try; a removed DLL's note goes");
    check(lines.size() == 4, "one log line per entry");
    const std::string written = guard_text(after);
    check(parse_guard(written).size() == 2 && written.find("skip\tTestMod.dll\tStats\t100-01D9\t2026-11-08\r\n") != std::string::npos &&
              written.rfind("# ", 0) == 0,
          "written back with a comment first, one line each, CRLF, and read back the same");
    check(parse_guard("").empty() && guard_text({}).find('\t') == std::string::npos, "an empty file: nothing left out");
}

// With an install's media folder: its UI.zip's keys, and the fragment check against its textures.
void real_media(const fs::path& media, int argc, char** argv) {
    Media m{media};
    AppKeys app;
    std::string why;
    if (!load_media(m, app, why)) {
        check(false, "the install's UI.zip is read");
        std::printf("     (%s)\n", why.c_str());
        return;
    }
    std::printf("     UI.zip: %zu keys from %d dictionaries\n", app.keys.size(), app.dictionaries);
    check(app.dictionaries == 43 && app.keys.size() > 1000, "the install's UI.zip: 43 merged dictionaries, over 1,000 keys");
    check(app.keys.count("A_Style_ContentControl_DefaultButtonBar") && app.keys.count("A_Style_ListBox_List_Default") &&
              !app.keys.count("A_Style_FZTextBlock_TileClubMemberCount"),
          "the keys mods' pages use are there; A_PauseMenu's own aren't");
    const TextureLookup tex = [&](const TextureSpot& s) { return m.texture(s); };
    FragmentCheck c;
    check(check_page_fragment(app, "", "<local:FZMaterial ImageSource=\"/ForzaUI;component/Textures/Anthem/Icons/Trophy_Icon_Medium.png\" />"
                                       "<local:FZMaterial ImageSource=\"/ForzaUI;component/Textures/Data_Bound/ControllerIcons/Controller_A.png\" />",
                              "", tex, c, why) &&
              c.images == 2 && c.images_unchecked == 0,
          "the install's textures: the trophy icon and a controller glyph are found");
    check(!check_page_fragment(app, "", "<Image Source=\"/ForzaUI;component/Textures/Anthem/Icons/No_Such_Icon.png\" />", "", tex, c, why),
          "the install's textures: an icon that isn't there is refused");
    for (int i = 1; i < argc; ++i) {
        const std::string page = slurp(argv[i]);
        const bool ok          = !page.empty() && check_page_fragment(app, "", page, "", tex, c, why);
        if (ok) {
            std::printf("ok   %s passes whole (%zu keys used, %zu own, %zu images)\n", argv[i], c.keys_used, c.own_keys, c.images);
        } else {
            std::printf("FAIL %s doesn't pass: %s\n", argv[i], page.empty() ? "it can't be read" : why.c_str());
            ++g_failed;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    platform_files();
    Media m{fs::current_path() / "fake-media"};
    fake_media(m.root);
    AppKeys app;
    zips_and_keys(m, app);
    spots();
    fragments(m, app);
    guard();
    if (const char* media = std::getenv("FH2_MEDIA"); media && *media) real_media(media, argc, argv);
    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
