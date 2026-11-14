// Which game Azimuth started (platform 2.8.0).
//
// Azimuth 1.0.1 runs several Forza games from one launcher: Forza Horizon 2,
// Forza Motorsport 5 and 6, and more, each set up in data\azimuth.ini as
// [title-<id>] with its folder. The platform loads when azimuth.exe starts,
// before the player picks one, and every one of those games' exes is called
// forza_durango_release_final.exe and is mapped at the same address,
// 0x140000000. So the exe's header tells them apart (TimeDateStamp and
// SizeOfImage, as find_game does), and once Azimuth has mapped a game that
// isn't FH2's 24 Nov 2015 exe, the platform leaves it alone: nothing is
// hooked or patched in it, game_base, wait_for_game and wait_for_code return
// NULL, mods from before the platform aren't loaded, and nothing keeps
// searching memory for FH2.
//
// Azimuth can also be told which game to start: --skip-launcher starts the
// one [launcher] game= names, and --game=DIR (a scripted run) the one in DIR.
// When that's another game, no mods load at all.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

constexpr uintptr_t kGamesBase = 0x140000000ull; // where Azimuth maps every game's exe
constexpr uint32_t kTimestamp  = 0x565501EDu;    // FH2's forza_durango_release_final.exe, 24 Nov 2015
constexpr wchar_t kFH2Title[]  = L"1a96545b";

std::atomic<int> g_state{static_cast<int>(Launched::not_yet)};
char g_name[192]; // the other game, written before g_state says so

struct Header {
    uint32_t stamp = 0, size = 0;
    bool fh2() const { return stamp == kTimestamp && size == kGameSizeOfImage; }
    bool operator==(const Header&) const = default;
};

bool parse_header(const char* p, std::size_t n, Header& h) {
    if (n < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x800 ||
        dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > n)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(p + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) || !nt->FileHeader.TimeDateStamp ||
        !nt->OptionalHeader.SizeOfImage)
        return false;
    h = {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage};
    return true;
}

bool read_header(const char* base, Header& h) {
    __try {
        return parse_header(base, 0x1000, h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool program_is_game_exe() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return n && n < MAX_PATH && !lstrcmpiW(fs::path{exe}.filename().c_str(), L"forza_durango_release_final.exe");
}

// A module the Windows loader loaded there (azimuth.exe, a DLL) rather than a
// game Azimuth mapped by hand. The program itself counts as the game when it's
// the game's exe (Mezcal).
bool loaded_module_at(const void* base) {
    static std::atomic<const void*> known{nullptr}; // the last one found, so the loader isn't asked every time
    if (known.load(std::memory_order_relaxed) == base) return true;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(base), &m))
        return false;
    static const bool game_exe = program_is_game_exe();
    if (m == GetModuleHandleW(nullptr) && game_exe) return false;
    known.store(base, std::memory_order_relaxed);
    return true;
}

// The header of the exe at the games' base address, if a game is mapped there.
bool header_at_base(Header& h) {
    const auto* base = reinterpret_cast<const char*>(kGamesBase);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(base, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || mbi.RegionSize < 0x1000)
        return false;
    if (!read_header(base, h)) return false;
    return mbi.Type != MEM_IMAGE || !loaded_module_at(mbi.AllocationBase);
}

// A game folder's exe on disk (in Mount, or the folder itself).
bool disk_header(const fs::path& folder, Header& h) {
    for (const fs::path& exe : {folder / L"Mount" / L"forza_durango_release_final.exe",
                               folder / L"forza_durango_release_final.exe"}) {
        HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        char head[0x1000];
        DWORD got     = 0;
        const bool ok = ReadFile(f, head, sizeof head, &got, nullptr) && parse_header(head, got, h);
        CloseHandle(f);
        if (ok) return true;
    }
    return false;
}

std::wstring ini_path() { return (azimuth_dir() / L"data" / L"azimuth.ini").wstring(); }

struct Title {
    std::wstring id, name;
    fs::path folder;
};

// azimuth.ini's [title-<id>] sections.
std::vector<Title> titles() {
    const std::wstring ini = ini_path();
    std::vector<wchar_t> names(32768);
    const DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), ini.c_str());
    std::vector<Title> out;
    for (const wchar_t* p = names.data(); p < names.data() + n && *p; p += std::wcslen(p) + 1) {
        if (_wcsnicmp(p, L"title-", 6)) continue;
        wchar_t folder[MAX_PATH]{}, name[128]{};
        GetPrivateProfileStringW(p, L"folder", L"", folder, MAX_PATH, ini.c_str());
        GetPrivateProfileStringW(p, L"name", L"", name, 128, ini.c_str());
        fs::path f{folder};
        if (!f.empty() && f.is_relative()) f = azimuth_dir() / f;
        out.push_back({p + 6, name, f});
    }
    return out;
}

std::string title_name(const Title& t) {
    return (t.name.empty() ? std::string("a game") : narrow(t.name)) + " (title " + narrow(t.id) + ")";
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%X", v);
    return b;
}

// What a game exe that isn't FH2's 24 Nov 2015 one is, by the titles in azimuth.ini.
struct Described {
    std::string name;
    bool fh2_title = false; // FH2's folder, with another exe in it
};
Described describe(const Header& h) {
    const std::string fingerprint = "TimeDateStamp " + hex(h.stamp) + ", SizeOfImage " + hex(h.size);
    for (const auto& t : titles()) {
        Header d;
        if (t.folder.empty() || !disk_header(t.folder, d) || !(d == h)) continue;
        if (!_wcsicmp(t.id.c_str(), kFH2Title))
            return {title_name(t) + ", but not the 24 Nov 2015 exe the mods are made for (" + fingerprint + ")", true};
        return {title_name(t)};
    }
    return {"a game exe that isn't one of the titles in data\\azimuth.ini (" + fingerprint + ")"};
}

void settle_other(const std::string& name) {
    lstrcpynA(g_name, name.c_str(), sizeof g_name);
    g_state.store(static_cast<int>(Launched::other), std::memory_order_release);
}

// Azimuth's command line, as CommandLineToArgvW splits it (shell32, looked up
// here so the platform doesn't import it).
std::vector<std::wstring> arguments() {
    std::vector<std::wstring> out;
    using Split = LPWSTR*(WINAPI*)(LPCWSTR, int*);
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    auto split    = shell ? reinterpret_cast<Split>(GetProcAddress(shell, "CommandLineToArgvW")) : nullptr;
    int argc      = 0;
    if (LPWSTR* argv = split ? split(GetCommandLineW(), &argc) : nullptr) {
        for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return out;
}

} // namespace

Launched launched_game() { return static_cast<Launched>(g_state.load(std::memory_order_acquire)); }

const char* launched_game_name() { return launched_game() == Launched::other ? g_name : ""; }

fs::path command_line_game_folder() {
    for (const auto& a : arguments())
        if (!_wcsnicmp(a.c_str(), L"--game=", 7) && a.size() > 7) {
            fs::path dir{a.substr(7)};
            std::error_code ec;
            return dir.is_relative() ? fs::absolute(dir, ec) : dir;
        }
    return {};
}

bool holds_fh2_exe(const fs::path& folder) {
    Header h;
    return !folder.empty() && disk_header(folder, h) && h.fh2();
}

std::string other_game_on_command_line() {
    bool skip = false;
    for (const auto& a : arguments())
        if (!_wcsicmp(a.c_str(), L"--skip-launcher")) skip = true;
    std::string name, how;
    if (const fs::path dir = command_line_game_folder(); !dir.empty()) {
        Header h;
        if (!disk_header(dir, h) || h.fh2()) return {};
        name = describe(h).name;
        how  = "--game=" + narrow(dir.wstring());
    } else if (skip) {
        wchar_t id[32]{};
        GetPrivateProfileStringW(L"launcher", L"game", L"", id, 32, ini_path().c_str());
        if (!id[0] || !_wcsicmp(id, kFH2Title)) return {};
        name = "title " + narrow(id);
        for (const auto& t : titles())
            if (!_wcsicmp(t.id.c_str(), id)) name = title_name(t);
        how = "--skip-launcher, and [launcher] game=" + narrow(id) + " in data\\azimuth.ini";
    } else {
        return {};
    }
    settle_other(name);
    return name + ", from " + how;
}

std::string other_game_at(const void* p) {
    Header h;
    if (!header_at_base(h) || h.fh2()) return {};
    const auto* c    = static_cast<const char*>(p);
    const auto* base = reinterpret_cast<const char*>(kGamesBase);
    if (c < base || c >= base + h.size) return {};
    return launched_game() == Launched::other ? std::string(g_name) : "a game that isn't Forza Horizon 2's 24 Nov 2015 exe";
}

void watch_launched_game(std::function<void()> fh2, std::function<void()> other) {
    std::thread([fh2 = std::move(fh2), other = std::move(other)] {
        // A header has to stay the same for 50 ms before it counts as another
        // game, so one read while Azimuth is still copying it in can't.
        Header seen;
        ULONGLONG since = 0;
        for (unsigned i = 0;; ++i) {
            if (launched_game() == Launched::other) break; // settled from the command line
            Header h;
            if (header_at_base(h)) {
                if (h.fh2()) {
                    g_state.store(static_cast<int>(Launched::fh2), std::memory_order_release);
                    plog("game: Azimuth started Forza Horizon 2 (the 24 Nov 2015 exe the mods are made for)");
                    fh2();
                    return;
                }
                if (!(h == seen)) {
                    seen  = h;
                    since = GetTickCount64();
                } else if (GetTickCount64() - since >= 50) {
                    const Described d = describe(h);
                    settle_other(d.name);
                    plog("game: Azimuth started " + d.name + (d.fh2_title ? "" : ", not Forza Horizon 2") +
                         ". The platform and its mods leave it alone: nothing is hooked or patched in it, and "
                         "mods waiting for the game are told it isn't there");
                    break;
                }
            } else {
                seen = {};
            }
            // FH2 mapped somewhere else (not seen so far, but find_game allows for it).
            if (i % 200 == 199 && find_game()) {
                g_state.store(static_cast<int>(Launched::fh2), std::memory_order_release);
                plog("game: Azimuth started Forza Horizon 2 (the 24 Nov 2015 exe the mods are made for)");
                fh2();
                return;
            }
            Sleep(5);
        }
        other();
    }).detach();
}

// ---- the title gate (platform 2.9.0) ------------------------------------------------
//
// The platform loads as azimuth.exe starts, before the player picks a game in the launcher, and it used to load
// every FH2 mod there and then. When the pick was another game, the platform kept its own hooks out of it, but the
// mods' DLLs were already in the process: their DllMain and fh2mod_init had run, with whatever they start outside
// the platform (their own hooks on input or the window, threads, overlays).
//
// So in an Azimuth folder set up for another game besides FH2, the loader waits here for the pick:
//  - another game: no mod is loaded this session;
//  - FH2: the mods load now. They have to be ready before the game runs, since it reads its string tables, files
//    and database once as it starts, so Azimuth is held meanwhile. Its import of VirtualProtect is pointed at
//    hold_until_ready, and the first time Azimuth makes part of the game executable, that call returns only once
//    every fh2mod_init has. Only azimuth.exe's own import slot is changed.
//
// Mods load as Azimuth starts, as before, when FH2 is the only title set up, when Azimuth was told which game to
// start (--skip-launcher, --game=), under Wine and Proton (the game isn't always mapped at 0x140000000 there), and
// when azimuth.exe has no VirtualProtect import to hold it by.
namespace {

using ProtectFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
ProtectFn g_azimuth_protect = nullptr; // what Azimuth's VirtualProtect import pointed at
HANDLE g_gate_ready         = nullptr; // the loader's event: every fh2mod_init has returned
std::atomic<bool> g_gate_held{false};

BOOL WINAPI hold_until_ready(LPVOID at, SIZE_T n, DWORD protect, PDWORD old) {
    const BOOL ok   = g_azimuth_protect(at, n, protect, old);
    const DWORD err = GetLastError();
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION mbi{};
    if (ok && (protect & kExecutable) && launched_game() != Launched::other &&
        VirtualQuery(at, &mbi, sizeof mbi) == sizeof mbi &&
        mbi.AllocationBase == reinterpret_cast<void*>(kGamesBase) && mbi.Type != MEM_IMAGE) {
        // A code section is in by now, so the header is whole and FH2's counts at once. Another game's is left to
        // the loader's 50 ms rule.
        Header h;
        if (launched_game() == Launched::not_yet && header_at_base(h) && h.fh2())
            g_state.store(static_cast<int>(Launched::fh2), std::memory_order_release);
        if (launched_game() == Launched::fh2 && WaitForSingleObject(g_gate_ready, 0) != WAIT_OBJECT_0) {
            const bool first   = !g_gate_held.exchange(true); // Azimuth may get here on more than one thread
            const ULONGLONG t0 = GetTickCount64();
            if (first) plog("title gate: Azimuth is held at the game's start until the mods are ready");
            const bool ready = WaitForSingleObject(g_gate_ready, 120000) == WAIT_OBJECT_0;
            if (first)
                plog(ready ? "title gate: the mods were ready " + std::to_string(GetTickCount64() - t0) +
                                 " ms later; the game starts"
                           : "title gate: the mods weren't ready after 2 minutes; the game starts without waiting "
                             "for them any longer");
        }
    }
    SetLastError(err);
    return ok;
}

// Points azimuth.exe's import of VirtualProtect at hold_until_ready.
bool hold_azimuth() {
    auto* image     = reinterpret_cast<char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;
    for (auto* dll = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + dir.VirtualAddress); dll->Name; ++dll) {
        if (!dll->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(image + dll->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(image + dll->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), "VirtualProtect")) continue;
            auto* slot        = reinterpret_cast<void**>(&slots->u1.Function);
            g_azimuth_protect = reinterpret_cast<ProtectFn>(*slot);
            DWORD old         = 0;
            if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&hold_until_ready));
            VirtualProtect(slot, sizeof *slot, old, &old);
            return true;
        }
    }
    return false;
}

} // namespace

bool wait_for_launcher_pick(void* mods_ready) {
    for (const auto& a : arguments())
        if (!_wcsicmp(a.c_str(), L"--skip-launcher") || !_wcsnicmp(a.c_str(), L"--game=", 7)) return true;
    int others = 0;
    for (const auto& t : titles())
        if (_wcsicmp(t.id.c_str(), kFH2Title)) ++others;
    if (!others) return true;
    const std::string folder = "title gate: this Azimuth folder is set up for " + std::to_string(others) +
                               (others == 1 ? " other game" : " other games") + " besides Forza Horizon 2";
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) {
        plog(folder + ". Under Wine and Proton the mods load now all the same, as before");
        return true;
    }
    g_gate_ready = static_cast<HANDLE>(mods_ready);
    if (!hold_azimuth()) {
        plog(folder + ", but azimuth.exe has no VirtualProtect import to hold the game by: the mods load now, as "
                      "before");
        return true;
    }
    plog(folder + ", so no mod is loaded until a game is picked");
    // As watch_launched_game: another game's header has to stay the same for 50 ms before it counts.
    Header seen;
    ULONGLONG since = 0;
    for (unsigned i = 0; launched_game() == Launched::not_yet; ++i) {
        Header h;
        if (!header_at_base(h)) {
            seen = {};
        } else if (h.fh2()) {
            g_state.store(static_cast<int>(Launched::fh2), std::memory_order_release);
        } else if (!(h == seen)) {
            seen  = h;
            since = GetTickCount64();
        } else if (GetTickCount64() - since >= 50) {
            settle_other(describe(h).name);
        }
        if (i % 200 == 199 && find_game()) g_state.store(static_cast<int>(Launched::fh2), std::memory_order_release);
        if (launched_game() == Launched::not_yet) Sleep(5);
    }
    if (launched_game() == Launched::other) return false;
    plog("title gate: Forza Horizon 2 was picked; loading the mods");
    return true;
}

} // namespace fh2platform
