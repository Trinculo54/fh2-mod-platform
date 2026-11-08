// What's different when the game runs under Wine or Proton: see wine.hpp.
#include "wine.hpp"

#include <algorithm>
#include <cctype>

namespace fh2platform {

bool same_pointer(uint64_t on_disk, uint64_t in_memory, uint64_t file_base, uint64_t mapped_base,
                  uint64_t image_size) {
    if (on_disk < file_base || on_disk - file_base >= image_size) return false;
    // Unsigned arithmetic wraps, so this holds for an image mapped below its preferred base too
    // (Proton has put it at 0x14690000, 0x18940000 and 0x19D40000).
    return in_memory == on_disk + (mapped_base - file_base);
}

// As Wine's parse_load_order (dlls/ntdll/loadorder.c): words separated by commas, spaces or tabs, of
// which only the first letter counts.
LoadOrder parse_load_order(const std::string& order) {
    for (std::size_t at = order.find_first_not_of(", \t"); at != std::string::npos;
         at = order.find_first_not_of(", \t", order.find_first_of(", \t", at))) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(order[at])));
        if (c == 'n') return LoadOrder::native_first;
        if (c == 'b') return LoadOrder::builtin_first;
    }
    return LoadOrder::disabled;
}

LoadOrder env_load_order(const std::string& overrides, const std::string& name) {
    const auto same = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    LoadOrder got = LoadOrder::unsaid;
    std::size_t at = 0;
    while (at < overrides.size()) {
        std::size_t end = overrides.find(';', at);
        if (end == std::string::npos) end = overrides.size();
        const std::string entry = overrides.substr(at, end - at);
        at = end + 1;
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        const LoadOrder order = parse_load_order(entry.substr(eq + 1));
        for (std::size_t n = 0; n < eq;) {
            std::size_t comma = entry.find(',', n);
            if (comma == std::string::npos || comma > eq) comma = eq;
            if (same(entry.substr(n, comma - n), name)) got = order;
            n = comma + 1;
        }
    }
    return got;
}

std::vector<std::string> wine_override_names(const std::string& dll) {
    std::string path = dll;
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".dll") path.resize(path.size() - 4);
    }
    const std::size_t slash = path.find_last_of("\\/");
    const std::string name  = slash == std::string::npos ? path : path.substr(slash + 1);
    if (slash == std::string::npos) return {"*" + name, name};
    return {path, "*" + name, name};
}

LoadOrder wine_load_order(const std::vector<std::string>& names, const LoadOrderSources& from) {
    for (const std::string& name : names)
        for (const auto* source : {&from.env, &from.app, &from.global})
            if (*source)
                if (const LoadOrder o = (*source)(name); o != LoadOrder::unsaid) return o;
    return LoadOrder::unsaid;
}

} // namespace fh2platform
