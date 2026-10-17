#include "updates.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <type_traits>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

constexpr long long kCheckEverySecs = 6 * 60 * 60;

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

std::string strip_v(std::string_view v) {
    while (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.remove_prefix(1);
    return std::string{v};
}

fs::path notices_dir(const fs::path& azimuth) { return azimuth / "fh2-mods" / "notices"; }

// WinHTTP, loaded from System32 on first use (see CMakeLists.txt).
struct WinHttp {
    decltype(&WinHttpOpen) Open;
    decltype(&WinHttpSetTimeouts) SetTimeouts;
    decltype(&WinHttpConnect) Connect;
    decltype(&WinHttpOpenRequest) OpenRequest;
    decltype(&WinHttpSendRequest) SendRequest;
    decltype(&WinHttpReceiveResponse) ReceiveResponse;
    decltype(&WinHttpQueryHeaders) QueryHeaders;
    decltype(&WinHttpQueryDataAvailable) QueryDataAvailable;
    decltype(&WinHttpReadData) ReadData;
    decltype(&WinHttpCloseHandle) CloseHandle;
    bool ok = false;
};

const WinHttp& winhttp() {
    static const WinHttp w = [] {
        WinHttp x{};
        HMODULE h = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) return x;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(h, name));
            return fn != nullptr;
        };
        x.ok = get(x.Open, "WinHttpOpen") && get(x.SetTimeouts, "WinHttpSetTimeouts") &&
               get(x.Connect, "WinHttpConnect") && get(x.OpenRequest, "WinHttpOpenRequest") &&
               get(x.SendRequest, "WinHttpSendRequest") && get(x.ReceiveResponse, "WinHttpReceiveResponse") &&
               get(x.QueryHeaders, "WinHttpQueryHeaders") && get(x.QueryDataAvailable, "WinHttpQueryDataAvailable") &&
               get(x.ReadData, "WinHttpReadData") && get(x.CloseHandle, "WinHttpCloseHandle");
        return x;
    }();
    return w;
}

std::string https_get(const std::wstring& host, const std::wstring& path, const std::wstring& agent) {
    std::string body;
    const auto& W = winhttp();
    if (!W.ok) return body;
    HINTERNET s = W.Open(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    W.SetTimeouts(s, 5000, 5000, 8000, 8000);
    HINTERNET c = W.Connect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? W.OpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    DWORD status = 0, len = sizeof(status);
    if (r &&
        W.SendRequest(r, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        W.ReceiveResponse(r, nullptr) &&
        W.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &len, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!W.QueryDataAvailable(r, &avail) || !avail || body.size() > (1 << 20)) break;
            std::string chunk(avail, '\0');
            if (!W.ReadData(r, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
    }
    if (r) W.CloseHandle(r);
    if (c) W.CloseHandle(c);
    W.CloseHandle(s);
    return body;
}

std::string tag_from_json(const std::string& j) {
    const auto k = j.find("\"tag_name\"");
    if (k == std::string::npos) return {};
    const auto q1 = j.find('"', j.find(':', k) + 1);
    const auto q2 = q1 == std::string::npos ? q1 : j.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 - q1 > 64) return {};
    return j.substr(q1 + 1, q2 - q1 - 1);
}

} // namespace

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

int compare_versions(std::string_view a, std::string_view b) {
    const std::string x = strip_v(a), y = strip_v(b);
    std::size_t i = 0, j = 0;
    for (int part = 0; part < 4; ++part) {
        long long p = 0, q = 0;
        while (i < x.size() && x[i] >= '0' && x[i] <= '9') p = p * 10 + (x[i++] - '0');
        while (j < y.size() && y[j] >= '0' && y[j] <= '9') q = q * 10 + (y[j++] - '0');
        if (p != q) return p < q ? -1 : 1;
        if (i < x.size() && x[i] == '.') ++i;
        if (j < y.size() && y[j] == '.') ++j;
    }
    return 0;
}

bool cache_still_good(long long checked, long long now, const std::string& latest, const std::string& cached_for,
                      const std::string& version) {
    if (latest.empty() || now - checked < 0 || now - checked >= kCheckEverySecs) return false;
    // A check from before this version was installed (or from a platform
    // that didn't note the version) says nothing about releases since.
    if (strip_v(cached_for) != strip_v(version)) return false;
    return compare_versions(latest, version) >= 0;
}

std::string latest_release(const fs::path& azimuth, const std::wstring& name, const std::string& repo,
                           const std::string& agent_version, bool* from_cache) {
    std::error_code ec;
    const auto file = notices_dir(azimuth) / (name + L".check");
    long long checked = 0;
    std::string latest, cached_for;
    {
        std::ifstream in{file};
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.rfind("checked=", 0) == 0) checked = std::atoll(line.c_str() + 8);
            if (line.rfind("latest=", 0) == 0) latest = line.substr(7);
            if (line.rfind("version=", 0) == 0) cached_for = line.substr(8);
        }
    }
    const long long now = static_cast<long long>(std::time(nullptr));
    if (from_cache) *from_cache = true;
    if (cache_still_good(checked, now, latest, cached_for, agent_version)) return latest;

    const auto tag = tag_from_json(https_get(L"api.github.com", L"/repos/" + widen(repo) + L"/releases/latest",
                                             name + L"/" + widen(agent_version)));
    if (tag.empty()) return latest; // offline: keep what we knew
    if (from_cache) *from_cache = false;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream{file, std::ios::trunc} << "checked=" << now << "\nlatest=" << tag << "\nversion=" << agent_version
                                         << "\n";
    return tag;
}

void write_notice_file(const fs::path& azimuth, const std::wstring& name, const std::wstring& dll,
                       const std::vector<std::string>& lines) {
    std::error_code ec;
    const auto f = notices_dir(azimuth) / (name + L".txt");
    if (lines.empty()) {
        fs::remove(f, ec);
        return;
    }
    fs::create_directories(f.parent_path(), ec);
    std::ofstream out{f, std::ios::binary | std::ios::trunc};
    out << narrow(dll) << "\n";
    for (const auto& l : lines) out << l << "\n";
}

namespace {

std::vector<std::string> notice_lines(const fs::path& azimuth) {
    std::error_code ec;
    std::vector<std::pair<std::wstring, std::vector<std::string>>> notes; // sorted by mod name
    for (const auto& e : fs::directory_iterator(notices_dir(azimuth), ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != L".txt") continue;
        std::ifstream in{e.path()};
        std::string dll, text;
        std::getline(in, dll);
        dll = trim(dll);
        if (!dll.empty() && !fs::exists(azimuth / "fh2-mods" / widen(dll), ec)) continue; // mod removed
        std::vector<std::string> lines;
        while (std::getline(in, text))
            if (!(text = trim(text)).empty()) lines.push_back(text);
        if (!lines.empty()) notes.emplace_back(e.path().stem().wstring(), lines);
    }
    std::sort(notes.begin(), notes.end());
    std::vector<std::string> all;
    for (const auto& [_, lines] : notes) all.insert(all.end(), lines.begin(), lines.end());
    return all;
}

} // namespace

bool any_notices(const fs::path& azimuth) { return !notice_lines(azimuth).empty(); }

std::wstring title_with_notices(const fs::path& azimuth, const std::wstring& to_start) {
    // "TO START" is its own text box beside "PRESS (A)", centred vertically
    // on it. Blank lines above balance the notice lines below so it stays
    // level with the button and the notices stack underneath; one extra
    // blank line evens out the box sitting a little high (checked in game).
    std::wstring above, below;
    for (const auto& l : notice_lines(azimuth)) {
        above += L"\n";
        below += L"\n" + widen(l);
    }
    if (below.empty()) return to_start;
    return above + L"\n" + to_start + below;
}

} // namespace fh2platform
