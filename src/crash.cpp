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
#include "platform.hpp"

#include <windows.h>

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

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, const void*, const void*, const void*);
struct DumpExceptionInfo {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};

void write_dump(const wchar_t* path, EXCEPTION_POINTERS* ep) {
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto fn = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                            reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                      : nullptr;
    if (!fn) return;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DumpExceptionInfo info{GetCurrentThreadId(), ep, FALSE};
    // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo: small,
    // but with what the stack points at.
    if (!fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40 | 0x1000, &info, nullptr, nullptr)) {
        // Older dbghelp versions don't know the extra flags: a plain dump.
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0, &info, nullptr, nullptr);
    }
    CloseHandle(f);
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
    if (blamed >= 0)
        o.put("the crash came from %ls", g_dlls[blamed].name);
    else
        o.put("no mod was on the walked call stack%s%s", list[0] ? "; mod code referenced on the stack: " : "",
              list);

    if (o.file != INVALID_HANDLE_VALUE) CloseHandle(o.file);
    std::swprintf(path, sizeof path / sizeof *path, L"%ls.dmp", base);
    write_dump(path, ep);
    log_line("", "crash report saved in fh2-mods\\crashes");

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
    g_prev = SetUnhandledExceptionFilter(on_crash);
    if (g_prev == on_crash) g_prev = nullptr;
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
