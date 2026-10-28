// Crash reports. When the process is about to die from an unhandled
// exception, write what happened to fh2-mods\crashes\crash-<time>.txt (and
// platform.log): the exception, where it happened as module+offset, the call
// stack as far as it can be walked, and which mod, if any, was involved.
// A minidump goes next to it, for anyone with a debugger. The handler only
// observes: it then passes the crash on to whatever handler was there
// before (Azimuth's, or Windows').
//
// Written to work with as little as possible of the C++ runtime, since the
// heap may be what broke: fixed buffers, WriteFile.
//
// From 2.8.0 the minidump has the writable data of the DLLs the platform
// loaded (the mods, fh2wheel and the platform itself), where a mod's state
// starts, but not other modules', so it stays small; memory mods add with
// add_dump_memory; and with fh2-mods\platform-dump.txt saying "full", the
// whole process (several GB under Azimuth: only the newest is kept). The
// report names each of those DLLs' PDB (GUID and age), for symbols later.
#include "platform.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace fh2platform {

namespace {

constexpr int kMaxDlls = 64;
struct Dll {
    wchar_t name[64];
    const char* base;
    std::size_t size;
};
Dll g_dlls[kMaxDlls];
int g_ndlls = 0;
wchar_t g_dir[MAX_PATH];
// add_dump_memory: filled under g_ranges_mu, published by g_nranges.
constexpr int kMaxDumpRanges = 64;
struct Range {
    const void* p;
    std::size_t n;
};
Range g_ranges[kMaxDumpRanges];
std::atomic<int> g_nranges{0};
std::atomic_flag g_ranges_mu = ATOMIC_FLAG_INIT;
bool g_full_dump = false; // fh2-mods\platform-dump.txt
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
std::atomic<bool> g_in_crash{false};
LONG WINAPI on_crash(EXCEPTION_POINTERS* ep);

struct Out {
    HANDLE file = INVALID_HANDLE_VALUE;
    char line[512];
    void put(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(line, sizeof line - 2, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (n > static_cast<int>(sizeof line) - 3) n = sizeof line - 3;
        line[n] = '\r';
        line[n + 1] = '\n';
        DWORD w = 0;
        if (file != INVALID_HANDLE_VALUE) WriteFile(file, line, static_cast<DWORD>(n + 2), &w, nullptr);
        line[n] = '\0';
        log_line("", line);
    }
};

// Which mod DLL `p` is in, or -1.
int mod_at(const void* p) {
    const char* c = static_cast<const char*>(p);
    for (int i = 0; i < g_ndlls; ++i)
        if (c >= g_dlls[i].base && c < g_dlls[i].base + g_dlls[i].size) return i;
    return -1;
}

void where(const void* p, char* out, std::size_t n) {
    const char* c = static_cast<const char*>(p);
    if (const int i = mod_at(p); i >= 0) {
        std::snprintf(out, n, "%ls+0x%llX", g_dlls[i].name,
                      static_cast<unsigned long long>(c - g_dlls[i].base));
        return;
    }
    if (const char* game = static_cast<const char*>(find_game()); game && c >= game && c < game + kGameSizeOfImage) {
        std::snprintf(out, n, "game+0x%llX", static_cast<unsigned long long>(c - game));
        return;
    }
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(p), &m)) {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < len; ++i)
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        std::snprintf(out, n, "%ls+0x%llX", len ? name : L"?",
                      static_cast<unsigned long long>(c - reinterpret_cast<const char*>(m)));
        return;
    }
    std::snprintf(out, n, "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
}

const char* exception_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xE06D7363: return "unhandled C++ exception";
    default: return "exception";
    }
}

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// What the dump callback kept, for the log.
struct DumpCounts {
    int dlls_with_data = 0;
    int ranges         = 0;
    int next_range     = 0;
};

// Keeps writable data (ModuleWriteDataSeg) only for the DLLs the platform loaded, and hands
// dbghelp the ranges mods added, one per MemoryCallback.
BOOL CALLBACK dump_callback(PVOID param, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    auto* c = static_cast<DumpCounts*>(param);
    if (!in || !out) return TRUE;
    if (in->CallbackType == ModuleCallback) {
        const char* base = reinterpret_cast<const char*>(in->Module.BaseOfImage);
        bool ours        = false;
        for (int i = 0; i < g_ndlls; ++i)
            if (g_dlls[i].base == base) ours = true;
        if (!ours) out->ModuleWriteFlags &= ~static_cast<ULONG>(ModuleWriteDataSeg);
        else if (out->ModuleWriteFlags & ModuleWriteDataSeg) ++c->dlls_with_data;
        return TRUE;
    }
    if (in->CallbackType == MemoryCallback) {
        const int n = g_nranges.load();
        while (c->next_range < n) {
            const Range& r = g_ranges[c->next_range++];
            if (!readable(r.p, r.n)) continue;
            out->MemoryBase = reinterpret_cast<ULONG64>(r.p);
            out->MemorySize = static_cast<ULONG>(r.n);
            ++c->ranges;
            return TRUE;
        }
        return FALSE;
    }
    return TRUE;
}

// A full dump replaces the last one: they're several GB each under Azimuth.
void remove_full_dumps() {
    wchar_t pattern[MAX_PATH + 32], one[MAX_PATH + 64];
    std::swprintf(pattern, sizeof pattern / sizeof *pattern, L"%ls\\crashes\\crash-*-full.dmp", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::swprintf(one, sizeof one / sizeof *one, L"%ls\\crashes\\%ls", g_dir, fd.cFileName);
        DeleteFileW(one);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void write_dump(const wchar_t* path, EXCEPTION_POINTERS* ep, DumpCounts* counts) {
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto fn = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                      : nullptr;
    if (!fn) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
    MINIDUMP_CALLBACK_INFORMATION cb{dump_callback, counts};
    // Small, but with what the stack points at and the mods' own data; or, switched on, everything.
    const auto type = static_cast<MINIDUMP_TYPE>(
        g_full_dump ? MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo
                    : MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
    if (!fn(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, &cb)) {
        // Older dbghelp versions don't know the extra flags: a plain dump.
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        *counts = DumpCounts{};
        fn(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &info, nullptr, nullptr);
    }
    CloseHandle(f);
}

// The PDB a DLL was built with, from its CodeView ("RSDS") debug record: what a debugger needs to
// find its symbols. Nothing if it has none (a stripped llvm-mingw build, or one without -gcodeview).
void put_pdb(Out& o, const Dll& d, bool& said) {
    IMAGE_DOS_HEADER dos;
    if (!d.base || !readable(d.base, sizeof dos)) return;
    std::memcpy(&dos, d.base, sizeof dos);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || !readable(d.base + dos.e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return;
    const auto* nt                  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(d.base + dos.e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || !dir.Size || !readable(d.base + dir.VirtualAddress, dir.Size)) return;
    const auto* e = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(d.base + dir.VirtualAddress);
    for (DWORD i = 0; i < dir.Size / sizeof *e; ++i) {
        if (e[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !e[i].AddressOfRawData || e[i].SizeOfData < 25) continue;
        const char* cv = d.base + e[i].AddressOfRawData;
        if (!readable(cv, e[i].SizeOfData) || std::memcmp(cv, "RSDS", 4) != 0) continue;
        GUID g;
        DWORD age;
        std::memcpy(&g, cv + 4, sizeof g);
        std::memcpy(&age, cv + 20, sizeof age);
        char name[200];
        const std::size_t len = std::min<std::size_t>(e[i].SizeOfData - 24, sizeof name - 1);
        std::memcpy(name, cv + 24, len);
        name[len] = '\0';
        if (!said) o.put("PDBs of the mods' DLLs (for symbols):");
        said = true;
        o.put("  %ls: %s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} age %lu", d.name, name, g.Data1, g.Data2,
              g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], age);
        return;
    }
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || g_in_crash.exchange(true))
        return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD& er = *ep->ExceptionRecord;

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t base[MAX_PATH + 64], path[MAX_PATH + 80];
    std::swprintf(base, sizeof base / sizeof *base, L"%ls\\crashes", g_dir);
    CreateDirectoryW(base, nullptr);
    std::swprintf(base, sizeof base / sizeof *base, L"%ls\\crashes\\crash-%04u%02u%02u-%02u%02u%02u", g_dir,
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::swprintf(path, sizeof path / sizeof *path, L"%ls.txt", base);

    Out o;
    o.file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    char at[160];
    where(er.ExceptionAddress, at, sizeof at);
    o.put("CRASH: %s (0x%08lX) at %s, thread %lu", exception_name(er.ExceptionCode), er.ExceptionCode, at,
          GetCurrentThreadId());
    if (launched_game() == Launched::other)
        o.put("game: %s, not the Forza Horizon 2 the platform and its mods are made for; they left it alone",
              launched_game_name());
    if ((er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er.NumberParameters >= 2)
        o.put("  %s address 0x%llX",
              er.ExceptionInformation[0] == 0 ? "reading" : er.ExceptionInformation[0] == 1 ? "writing" : "executing",
              static_cast<unsigned long long>(er.ExceptionInformation[1]));

    // Walk the stack with the unwind data of each module. Stops where a
    // frame has none it can find.
    int blamed = mod_at(er.ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
    o.put("call stack:");
    for (int i = 0; i < 40 && ctx.Rip; ++i) {
        char w[160];
        where(reinterpret_cast<void*>(ctx.Rip), w, sizeof w);
        o.put("  %2d  %s", i, w);
        if (blamed < 0) blamed = mod_at(reinterpret_cast<void*>(ctx.Rip));
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
        if (fe) {
            void* handler_data      = nullptr;
            DWORD64 establisher     = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fe, &ctx, &handler_data, &establisher, nullptr);
        } else {
            // A leaf function: the return address is on top of the stack.
            if (!readable(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
    }

    // The walk can stop early inside the game, so also list mod code
    // that return addresses on the stack point into.
    bool on_stack[kMaxDlls] = {};
    const auto* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
    for (int i = 0; i < 4096 && readable(sp + i, sizeof *sp); ++i)
        if (const int m = mod_at(reinterpret_cast<void*>(sp[i])); m >= 0) on_stack[m] = true;
    char list[400] = "";
    for (int i = 0; i < g_ndlls; ++i)
        if (on_stack[i]) {
            char one[80];
            std::snprintf(one, sizeof one, "%s%ls", list[0] ? ", " : "", g_dlls[i].name);
            strncat(list, one, sizeof list - std::strlen(list) - 1);
        }
    char hook[200];
    if (hook_at(er.ExceptionAddress, hook, sizeof hook))
        o.put("the crash was in %s", hook);
    if (blamed >= 0)
        o.put("the crash came from %ls", g_dlls[blamed].name);
    else
        o.put("no mod was on the walked call stack%s%s", list[0] ? "; mod code referenced on the stack: " : "",
              list);
    if (const int n = hook_count()) {
        o.put("hooks (* = on):");
        for (int i = 0; i < n; ++i) {
            bool on            = false;
            const char* line   = hook_line(i, &on);
            if (line) o.put("  %c %s", on ? '*' : ' ', line);
        }
    }

    bool said_pdbs = false;
    for (int i = 0; i < g_ndlls; ++i) put_pdb(o, g_dlls[i], said_pdbs);

    if (o.file != INVALID_HANDLE_VALUE) CloseHandle(o.file);
    if (g_full_dump) remove_full_dumps();
    std::swprintf(path, sizeof path / sizeof *path, g_full_dump ? L"%ls-full.dmp" : L"%ls.dmp", base);
    DumpCounts counts;
    write_dump(path, ep, &counts);
    char saved[200];
    std::snprintf(saved, sizeof saved,
                  "crash report saved in fh2-mods\\crashes, with a %s minidump (the data of %d of the mods' DLLs, and %d range(s) mods added)",
                  g_full_dump ? "full-memory" : "small", counts.dlls_with_data, counts.ranges);
    log_line("", saved);
    char report[64];
    std::snprintf(report, sizeof report, "crashes\\\\crash-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond);
    write_session_crashed(g_dir, report);

    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install_crash_handler(const std::vector<LoadedDll>& mods) {
    g_ndlls = 0;
    for (const auto& m : mods) {
        if (g_ndlls == kMaxDlls) break;
        Dll& d = g_dlls[g_ndlls++];
        lstrcpynW(d.name, m.name.c_str(), static_cast<int>(sizeof d.name / sizeof *d.name));
        d.base = m.base;
        d.size = m.size;
    }
    lstrcpynW(g_dir, (azimuth_dir() / L"fh2-mods").c_str(), MAX_PATH);
    // Keep the ten newest reports (a .txt and a .dmp each).
    std::error_code ec;
    std::vector<std::filesystem::path> old;
    for (const auto& e : std::filesystem::directory_iterator(azimuth_dir() / L"fh2-mods" / L"crashes", ec))
        if (e.path().filename().wstring().rfind(L"crash-", 0) == 0) old.push_back(e.path());
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; i + 20 < old.size(); ++i) std::filesystem::remove(old[i], ec);
    // fh2-mods\platform-dump.txt saying "full": a full-memory dump while chasing a crash.
    if (FILE* f = _wfopen((azimuth_dir() / L"fh2-mods" / L"platform-dump.txt").c_str(), L"r")) {
        char word[16] = {};
        g_full_dump   = std::fscanf(f, "%15s", word) == 1 && _stricmp(word, "full") == 0;
        std::fclose(f);
    }
    if (g_full_dump)
        plog("crash dumps: full memory (fh2-mods\\platform-dump.txt says so). They're several GB under Azimuth, so only the "
             "newest is kept");
    g_prev = SetUnhandledExceptionFilter(on_crash);
    if (g_prev == on_crash) g_prev = nullptr;
}

void add_dump_memory(FH2Mod* m, const void* p, std::size_t n) {
    if (!m) return;
    if (!p || !n || n > (64u << 20)) {
        log_line(m->name, "add_dump_memory: needs memory of 1 byte to 64 MB; not added");
        return;
    }
    while (g_ranges_mu.test_and_set(std::memory_order_acquire)) {}
    const int i = g_nranges.load();
    if (i < kMaxDumpRanges) {
        g_ranges[i] = {p, n};
        g_nranges.store(i + 1);
    }
    g_ranges_mu.clear(std::memory_order_release);
    if (i >= kMaxDumpRanges) log_line(m->name, "add_dump_memory: crash dumps already have 64 ranges from mods; not added");
    else log_line(m->name, "add_dump_memory: " + std::to_string(n) + " bytes go in crash dumps");
}

void add_crash_dll(const LoadedDll& m) {
    if (g_ndlls == kMaxDlls) return;
    Dll& d = g_dlls[g_ndlls];
    lstrcpynW(d.name, m.name.c_str(), static_cast<int>(sizeof d.name / sizeof *d.name));
    d.base = m.base;
    d.size = m.size;
    std::atomic_thread_fence(std::memory_order_release);
    ++g_ndlls; // only once it's filled in: a crash can read the list at any time
}

// Azimuth may install its own filter after we did; put ours back in front,
// passing crashes on to theirs.
void recheck_crash_handler() {
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(on_crash);
    if (cur != on_crash) {
        g_prev = cur;
        plog("crash reports: another crash handler was installed after the platform's; both run");
    }
}

} // namespace fh2platform
