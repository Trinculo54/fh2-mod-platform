// File and XAML safety (platform 2.9.0); see xaml_safety.hpp.
//
// Why the page check exists: the game builds a page from its XAML as it opens, and a {StaticResource K} whose key
// isn't there, or an image that isn't, crashes the game there and then, with nothing in any log. A page of a mod's
// own design (set_page_xaml) is the mod's XAML inside the platform's frame, so the platform checks it first, the way
// the HUD layer's template is checked (hud_template.cpp): against the keys every page sees (AnthemApp.xaml and the
// 43 dictionaries it merges), the three the frame adds, and the fragment's own. Keys a scene keeps in its own
// <Page.Resources> (A_PauseMenu's, A_HUDOptions' own items) aren't there, and neither are the FM5-era Common_* and
// Resources\*ResourceDictionary.xaml, which no page merges.
#include "xaml_safety.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace fh2platform {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

bool starts_ci(std::string_view s, std::string_view with) { return s.size() >= with.size() && lowered(s.substr(0, with.size())) == with; }
bool ends_ci(std::string_view s, std::string_view with) {
    return s.size() >= with.size() && lowered(s.substr(s.size() - with.size())) == with;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

// One tag of a XAML text.
struct Tag {
    std::string_view name, text;   // the element's name ("local:FZTextBlock"), and the whole tag
    std::string_view parent;       // the enclosing element's name ("" at the top level)
    int depth  = 0;                // 0 at the top level
    bool close = false, self = false;
};

// Calls f(tag) for every start, self-closing and end tag, skipping comments, <?...?>, <!...> and CDATA. False,
// with why, if the tags don't balance.
template <class F> bool each_tag(std::string_view s, F&& f, std::string& why) {
    std::vector<std::string_view> open;
    std::size_t i = 0;
    while ((i = s.find('<', i)) != std::string_view::npos) {
        const char* skip_to = s.compare(i, 4, "<!--") == 0        ? "-->"
                              : s.compare(i, 9, "<![CDATA[") == 0 ? "]]>"
                              : s.compare(i, 2, "<?") == 0        ? "?>"
                              : s.compare(i, 2, "<!") == 0        ? ">"
                                                                  : nullptr;
        if (skip_to) {
            const std::size_t e = s.find(skip_to, i + 2);
            if (e == std::string_view::npos) return why = "a comment or declaration isn't closed", false;
            i = e + std::strlen(skip_to);
            continue;
        }
        std::size_t e = i + 1;
        char quote    = 0;
        for (; e < s.size(); ++e) {
            const char c = s[e];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (e >= s.size()) return why = "a tag isn't closed", false;
        Tag t;
        t.close = s[i + 1] == '/';
        t.self  = !t.close && s[e - 1] == '/';
        std::size_t n0 = i + (t.close ? 2 : 1), n1 = n0;
        while (n1 < e && !space(s[n1]) && s[n1] != '/' && s[n1] != '>') ++n1;
        t.name = s.substr(n0, n1 - n0);
        t.text = s.substr(i, e + 1 - i);
        if (t.name.empty()) return why = "a tag has no name", false;
        if (t.close) {
            if (open.empty() || open.back() != t.name)
                return why = "</" + std::string(t.name) + "> closes " + (open.empty() ? std::string("nothing") : "<" + std::string(open.back()) + ">"),
                       false;
            open.pop_back();
        }
        t.depth  = static_cast<int>(open.size());
        t.parent = open.empty() ? std::string_view() : open.back();
        if (!t.close && !t.self) open.push_back(t.name);
        f(static_cast<const Tag&>(t));
        i = e + 1;
    }
    if (!open.empty()) return why = "<" + std::string(open.back()) + "> isn't closed", false;
    return true;
}

// Calls f(name, value) for each attribute of a start tag.
template <class F> void each_attr(std::string_view tag, F&& f) {
    std::size_t i = 1;
    while (i < tag.size() && !space(tag[i]) && tag[i] != '>' && tag[i] != '/') ++i;   // the element's name
    while (i < tag.size()) {
        while (i < tag.size() && space(tag[i])) ++i;
        const std::size_t n0 = i;
        while (i < tag.size() && tag[i] != '=' && !space(tag[i]) && tag[i] != '>' && tag[i] != '/') ++i;
        const std::string_view name = tag.substr(n0, i - n0);
        while (i < tag.size() && space(tag[i])) ++i;
        if (name.empty() || i >= tag.size() || tag[i] != '=') {
            if (name.empty()) ++i;
            continue;
        }
        ++i;
        while (i < tag.size() && space(tag[i])) ++i;
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) return;
        const char quote    = tag[i];
        const std::size_t e = tag.find(quote, i + 1);
        if (e == std::string_view::npos) return;
        f(name, tag.substr(i + 1, e - i - 1));
        i = e + 1;
    }
}

bool attr(std::string_view tag, std::string_view name, std::string_view& value) {
    bool found = false;
    each_attr(tag, [&](std::string_view n, std::string_view v) {
        if (!found && n == name) found = true, value = v;
    });
    return found;
}

// "local:FZTextBlock" -> "FZTextBlock"
std::string_view local_name(std::string_view n) {
    const std::size_t colon = n.find(':');
    return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

// The keys a dictionary gives pages: x:Key on the children of the document's own ResourceDictionary (the root,
// or Application.Resources' one), never one nested in a Style's or a template's own resources.
bool dict_keys(const std::string& xaml, std::set<std::string>& keys, std::string& why) {
    int dict = -1;   // the depth of the document's own ResourceDictionary while it's open
    return each_tag(
        xaml,
        [&](const Tag& t) {
            if (t.name == "ResourceDictionary" && !t.close && dict < 0 && (t.depth == 0 || (t.depth == 2 && t.parent == "Application.Resources"))) {
                if (!t.self) dict = t.depth;
                return;
            }
            if (t.close && t.name == "ResourceDictionary" && t.depth == dict) {
                dict = -1;
                return;
            }
            std::string_view key;
            if (dict >= 0 && !t.close && t.depth == dict + 1 && attr(t.text, "x:Key", key)) keys.emplace(key);
        },
        why);
}

// Every {StaticResource K} and {DynamicResource K} in one attribute value: f(K), or f("") for one without a key.
// Keys that are markup themselves ({x:Type Button}) can't be checked and are passed over.
template <class F> void each_resource_use(std::string_view v, F&& f) {
    if (v.substr(0, 2) == "{}") return;   // "{}" in front makes the rest plain text
    for (const char* kind : {"{StaticResource", "{DynamicResource"}) {
        const std::size_t klen = std::strlen(kind);
        for (std::size_t at = v.find(kind); at != std::string_view::npos; at = v.find(kind, at + 1)) {
            std::size_t b = at + klen;
            if (b < v.size() && !space(v[b]) && v[b] != '}') continue;   // {StaticResourceX}: some other name
            while (b < v.size() && space(v[b])) ++b;
            if (v.compare(b, 11, "ResourceKey") == 0) {
                const std::size_t eq = v.find('=', b);
                if (eq == std::string_view::npos) continue;
                b = eq + 1;
                while (b < v.size() && space(v[b])) ++b;
            }
            if (b < v.size() && v[b] == '{') continue;
            std::size_t e = b;
            while (e < v.size() && v[e] != '}' && v[e] != ',' && !space(v[e])) ++e;
            f(v.substr(b, e - b));
        }
    }
}

// Attributes that hold an image (the game's own XAML: Source, ImageSource, UriSource and Icon, and a Setter's Value
// for one of them).
bool image_property(std::string_view name) {
    const std::string_view n = local_name(name);
    return n == "Source" || n == "ImageSource" || n == "UriSource" || n == "Icon";
}

// A value that names a texture: one of the game's two path forms, or a file name with an image's extension.
// (A ResourceDictionary's Source, a .xaml, isn't one.)
bool texture_like(std::string_view v) {
    v = trimmed(v);
    if (v.empty() || v.front() == '{') return false;
    for (const char* ext : {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".dds", ".tga", ".swatchbin"})
        if (ends_ci(v, ext)) return true;
    return starts_ci(v, "/forzaui;component") || starts_ci(v, "/textures/") || starts_ci(v, "\\textures\\") || starts_ci(v, "pack:");
}
// Wherever it is, a value in one of the game's two path forms is a texture path.
bool texture_path_form(std::string_view v) {
    v = trimmed(v);
    return starts_ci(v, "/forzaui;component/") || starts_ci(v, "/forzaui;component\\") || starts_ci(v, "/textures/") ||
           starts_ci(v, "/textures\\") || starts_ci(v, "\\textures\\");
}

} // namespace

// ---- the platform's own UI files -------------------------------------------------------------------------------

const PlatformUiFile kPlatformUiFiles[] = {
    {"UI\\Scenes\\Anthem\\A_Hud.xaml", "the HUD notice about mods that didn't load, and the HUD layer"},
    {"UI\\Scenes\\Anthem\\A_Splash.xaml", "the title screen's mod entries"},
    {"UI\\Scenes\\Anthem\\A_PauseMenu.xaml", "the pause menu's MODS tab"},
    {"UI\\Scenes\\Anthem\\A_PauseOptions.xaml", "the Mods tile in the pause menu's Settings"},
    {"UI\\Scenes\\Anthem\\A_ModOptions.xaml", "the Mods page"},
    {"UI\\Scenes\\Anthem\\A_ModPage_NN.xaml", "the MODS tab's pages and the mods' pages"},
    {"stateflow\\pause.xml", "how the pause menu opens the MODS tab's pages"},
    {"stateflow\\ui_screens.xml", "the MODS tab's screens"},
};
const std::size_t kPlatformUiFileCount = sizeof kPlatformUiFiles / sizeof kPlatformUiFiles[0];

const PlatformUiFile* platform_ui_file(std::string_view media_path) {
    // the path's parts, lower case, as Windows reads them: no trailing dots or spaces
    std::vector<std::string> parts;
    std::string part;
    for (std::size_t i = 0; i <= media_path.size(); ++i) {
        const char c = i < media_path.size() ? media_path[i] : '/';
        if (c != '/' && c != '\\') {
            part += lower(c);
            continue;
        }
        while (!part.empty() && (part.back() == '.' || part.back() == ' ')) part.pop_back();
        if (!part.empty()) parts.push_back(part);
        part.clear();
    }
    if (!parts.empty() && parts[0] == "media") parts.erase(parts.begin());
    if (!parts.empty() && parts[0] == "ui.zip") parts[0] = "ui";
    if (!parts.empty() && parts[0] == "stateflow.zip") parts[0] = "stateflow";
    if (!parts.empty() && parts[0] == "ui") parts.erase(parts.begin());   // UI.zip's files: the path inside it
    if (parts.size() == 2 && parts[0] == "stateflow") {
        if (parts[1] == "pause.xml") return &kPlatformUiFiles[6];
        if (parts[1] == "ui_screens.xml") return &kPlatformUiFiles[7];
        return nullptr;
    }
    if (parts.size() != 3 || parts[0] != "scenes" || parts[1] != "anthem") return nullptr;
    const std::string& name = parts[2];
    for (std::size_t k = 0; k < 5; ++k) {
        const std::string_view listed = kPlatformUiFiles[k].path;
        if (name == lowered(listed.substr(listed.rfind('\\') + 1))) return &kPlatformUiFiles[k];
    }
    const std::string_view pre = "a_modpage_", ext = ".xaml";
    if (name.size() > pre.size() + ext.size() && name.compare(0, pre.size(), pre) == 0 && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
        bool digits = true;
        for (std::size_t i = pre.size(); i < name.size() - ext.size(); ++i) digits = digits && name[i] >= '0' && name[i] <= '9';
        if (digits) return &kPlatformUiFiles[5];
    }
    return nullptr;
}

// ---- the page check --------------------------------------------------------------------------------------------

const char* const kPageFrameKeys[] = {"FH2Mod_ShowIf", "FH2Mod_ShowIfHidden", "FH2Mod_HideIf"};
const std::size_t kPageFrameKeyCount = sizeof kPageFrameKeys / sizeof kPageFrameKeys[0];

bool load_app_keys(const std::function<std::string(const std::string&)>& read, AppKeys& out, std::string& why) {
    out = AppKeys{};
    const std::string app = read ? read("AnthemApp.xaml") : std::string();
    if (app.empty()) return why = "AnthemApp.xaml couldn't be read from UI.zip", false;
    std::string tag_why;
    if (!dict_keys(app, out.keys, tag_why)) return why = "AnthemApp.xaml's tags: " + tag_why, false;
    std::vector<std::string> merged;
    each_tag(
        app,
        [&](const Tag& t) {
            std::string_view src;
            if (!t.close && t.name == "ResourceDictionary" && attr(t.text, "Source", src)) merged.emplace_back(src);
        },
        tag_why);
    for (const std::string& path : merged) {
        const std::string d = read(path);
        if (d.empty()) return why = path + " couldn't be read from UI.zip", false;
        if (!dict_keys(d, out.keys, tag_why)) return why = path + "'s tags: " + tag_why, false;
        ++out.dictionaries;
    }
    return true;
}

std::vector<TextureSpot> texture_spots(std::string_view path) {
    std::string p(trimmed(path));
    for (char& c : p)
        if (c == '\\') c = '/';
    if (starts_ci(p, "/forzaui;component/")) p.erase(0, std::strlen("/forzaui;component"));
    if (!starts_ci(p, "/textures/") || !ends_ci(p, ".png") || p[p.size() - 5] == '/') return {};
    p = p.substr(std::strlen("/textures/"), p.size() - std::strlen("/textures/") - 4) + ".swatchbin";
    // every part a name: no empty part, "." or ".."
    for (std::size_t i = 0, start = 0; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') {
            const std::string_view part = std::string_view(p).substr(start, i - start);
            if (part.empty() || part == "." || part == "..") return {};
            start = i + 1;
        }
    const std::size_t slash = p.find('/');
    if (slash == std::string::npos) return {};
    const std::string top = lowered(p.substr(0, slash)), rest = p.substr(slash + 1);
    if (top == "anthem") return {{"Anthem.zip", rest}};
    if (top == "wpf") return {{"WPF.zip", rest}};
    if (top == "localized") return {{"Localized.zip", rest}};
    if (top == "data_bound") {
        // Data_Bound\<Zip>\<path>: media\ui\textures\data_bound\<Zip>.zip; else Data_Bound.zip itself
        std::vector<TextureSpot> spots;
        if (const std::size_t s = rest.find('/'); s != std::string::npos) spots.push_back({"data_bound\\" + rest.substr(0, s) + ".zip", rest.substr(s + 1)});
        spots.push_back({"Data_Bound.zip", rest});
        return spots;
    }
    return {};
}

std::string loose_texture_path(const TextureSpot& s) {
    std::string zip = s.zip;
    if (ends_ci(zip, ".zip")) zip.resize(zip.size() - 4);
    std::string p = "ui\\textures\\" + zip + "\\" + s.entry;
    for (char& c : p)
        if (c == '/') c = '\\';
    return p;
}

bool check_page_fragment(const AppKeys& app, std::string_view resources, std::string_view content, std::string_view help,
                         const TextureLookup& textures, FragmentCheck& out, std::string& why) {
    out = FragmentCheck{};
    struct Use {
        std::string what;    // the key or path
        const char* part;    // "resources", "content" or "help"
    };
    std::vector<Use> keys, images;
    std::set<std::string> own;
    const std::pair<const char*, std::string_view> parts[] = {{"resources", resources}, {"content", content}, {"help", help}};
    for (const auto& [part, text] : parts) {
        std::string tag_why;
        const bool ok = each_tag(
            text,
            [&](const Tag& t) {
                if (t.close) return;
                const std::string_view el = local_name(t.name);
                std::string_view property;
                const bool setter = el == "Setter" && attr(t.text, "Property", property);
                if (el == "StaticResource" || el == "DynamicResource") {   // the element form
                    std::string_view k;
                    if (attr(t.text, "ResourceKey", k) && !trimmed(k).empty() && trimmed(k).front() != '{') keys.push_back({std::string(trimmed(k)), part});
                }
                each_attr(t.text, [&](std::string_view n, std::string_view v) {
                    if (n == "x:Key") own.emplace(v);
                    each_resource_use(v, [&](std::string_view k) { keys.push_back({std::string(k), part}); });
                    const bool holds_image = image_property(n) || (setter && n == "Value" && image_property(property));
                    if ((holds_image && texture_like(v)) || texture_path_form(v)) images.push_back({std::string(trimmed(v)), part});
                });
                // a path as an element's text: <ImageSource x:Key="K">/ForzaUI;component/Textures/...</ImageSource>, as
                // Global_Textures.xaml has them
                if (!t.self) {
                    const std::size_t from = static_cast<std::size_t>(t.text.data() + t.text.size() - text.data());
                    const std::size_t to   = text.find('<', from);
                    const std::string_view inner = text.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from);
                    if (texture_path_form(inner) || ((el == "ImageSource" || el == "BitmapImage") && texture_like(inner)))
                        images.push_back({std::string(trimmed(inner)), part});
                }
            },
            tag_why);
        if (!ok) return why = std::string("the tags in the ") + part + " don't balance: " + tag_why, false;
    }
    out.own_keys = own.size();
    std::set<std::string> used;
    for (const Use& u : keys) {
        if (u.what.empty()) return why = std::string("a {StaticResource} or {DynamicResource} in the ") + u.part + " has no key", false;
        used.insert(u.what);
        if (own.count(u.what) || app.keys.count(u.what)) continue;
        bool frame = false;
        for (std::size_t k = 0; k < kPageFrameKeyCount; ++k) frame = frame || u.what == kPageFrameKeys[k];
        if (frame) continue;
        return why = "the resource key " + u.what + ", in the " + u.part +
                     ", isn't one of the game's shared resources, the page's own or the fragment's (a scene's own "
                     "resources, A_PauseMenu's say, aren't there: copy them into the fragment's resources)",
               false;
    }
    out.keys_used = used.size();
    std::map<std::string, int> seen;   // each distinct path once: 1 found, -1 its zip unreadable
    for (const Use& u : images) {
        if (seen.count(u.what)) continue;
        const std::vector<TextureSpot> spots = texture_spots(u.what);
        if (spots.empty())
            return why = "the image \"" + u.what + "\", in the " + u.part +
                         ", isn't one of the game's texture paths (\"/ForzaUI;component/Textures/<folder>/<name>.png\")",
                   false;
        int found = -1;
        std::string missing;   // the zips that were read and don't have it, for the reason
        for (const TextureSpot& s : spots) {
            const int r = textures ? textures(s) : -1;
            if (r == 1) {
                found = 1;
                break;
            }
            if (r == 0) {
                found = 0;
                missing += (missing.empty() ? "media\\ui\\textures\\" : ", nor media\\ui\\textures\\") + s.zip + " has no " + s.entry;
            }
        }
        if (found == 0)
            return why = "the image \"" + u.what + "\", in the " + u.part + ", isn't one of the game's textures (" + missing +
                         ", and no media folder has it as " + loose_texture_path(spots.back()) + ")",
                   false;
        seen[u.what] = found;
        ++out.images;
        if (found < 0) ++out.images_unchecked;
    }
    return true;
}

// ---- the page crash guard's state ------------------------------------------------------------------------------

std::vector<GuardEntry> parse_guard(std::string_view text) {
    std::vector<GuardEntry> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(at, nl - at);
        at = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        std::vector<std::string> f;
        for (std::size_t s = 0;;) {
            const std::size_t tab = line.find('\t', s);
            f.emplace_back(line.substr(s, tab == std::string_view::npos ? std::string_view::npos : tab - s));
            if (tab == std::string_view::npos) break;
            s = tab + 1;
        }
        if (f.size() < 4 || (f[0] != "opening" && f[0] != "skip") || f[1].empty() || f[2].empty()) continue;
        GuardEntry e{f[0], f[1], f[2], f[3], f.size() > 4 ? f[4] : ""};
        if (f.size() > 5) e.sessions = std::max(1, std::atoi(f[5].c_str()));
        if (f.size() > 6) e.marked = std::strtoull(f[6].c_str(), nullptr, 16);
        bool dup = false;
        for (GuardEntry& o : out)
            if (lowered(o.dll) == lowered(e.dll) && o.page == e.page) o = e, dup = true;   // the last one counts
        if (!dup) out.push_back(e);
    }
    return out;
}

std::string guard_text(const std::vector<GuardEntry>& entries) {
    std::string s = "# FH2 Mod Platform: pages of the mods' own design that were opening when the game stopped, and ones left out\r\n"
                    "# since, until their mod's DLL changes (platform.log says why). Delete a line to try that page again.\r\n";
    for (const GuardEntry& e : entries) {
        char tail[48];
        std::snprintf(tail, sizeof tail, "\t%d\t%016llX\r\n", e.sessions, static_cast<unsigned long long>(e.marked));
        s += e.state + "\t" + e.dll + "\t" + e.page + "\t" + e.stamp + "\t" + e.date + tail;
    }
    return s;
}

std::vector<GuardEntry> guard_at_start(const std::vector<GuardEntry>& last, const std::function<std::string(const std::string&)>& stamp_of,
                                       const CrashReport& newest, std::uint64_t file_time, std::vector<GuardEntry>& carried,
                                       std::vector<std::string>& lines) {
    std::vector<GuardEntry> out;
    carried.clear();
    for (GuardEntry e : last) {
        const std::string now = stamp_of ? stamp_of(e.dll) : std::string();
        const std::string page = e.dll + "'s page " + e.page;
        if (now.empty()) {
            lines.push_back(e.dll + " isn't in fh2-mods any more, so the note about " + page + " is dropped");
            continue;
        }
        if (now != e.stamp) {
            lines.push_back(e.dll + " has changed since " + page + (e.state == "opening" ? " was opening when the game stopped" : " was left out") +
                            ", so the page gets another try");
            continue;
        }
        if (e.state == "opening") {
            // as the HUD layer's crash marker: a crash report since the mark, or two sessions running
            const std::uint64_t marked = e.marked ? e.marked : file_time;
            if (!newest.name.empty() && newest.time > marked) {
                e.state = "skip";
                lines.push_back("the game crashed while " + page + " was opening last session (crash report " + newest.name +
                                "); the page is left out until " + e.dll + " changes (an update or a reinstall)");
            } else if (e.sessions >= 2) {
                e.state = "skip";
                lines.push_back("the game stopped while " + page + " was opening in " + std::to_string(e.sessions) +
                                " sessions running, and it was never up for 10 s; the page is left out until " + e.dll +
                                " changes (an update or a reinstall)");
            } else {
                carried.push_back(e);
                lines.push_back("the game stopped while " + page + " was opening last session, but there's no crash report since, so the "
                                "page stays in (it's left out if the game crashes opening it, or stops while it's opening again this session)");
                continue;
            }
        } else {
            lines.push_back(page + " is still left out: the game stopped while it was opening" + (e.date.empty() ? "" : " on " + e.date) +
                            ", and " + e.dll + " hasn't changed since");
        }
        out.push_back(e);
    }
    return out;
}

} // namespace fh2platform
