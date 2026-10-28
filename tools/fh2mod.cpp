// fh2mod.exe: runs a mod's command in the running game, through the platform's command pipe
// (\\.\pipe\fh2mod-<process id>, see docs/api.md, Commands).
//   fh2mod list                          every mod's commands
//   fh2mod <mod> <command> [args ...]    one of them: fh2mod WreckingBall debris clear
//   fh2mod --pid <id> ...                the game with that process id, when more than one runs
// It prints the reply, and exits with 0, or 1 if the reply is an error or no game answered.
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The pipes of running games: fh2mod-<process id>.
std::vector<std::wstring> game_pipes() {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n.rfind(L"fh2mod-", 0) == 0 && n.size() > 7 && n.find_first_not_of(L"0123456789", 7) == std::wstring::npos)
            out.push_back(n.substr(7));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

int usage() {
    std::fputs("usage: fh2mod list\n"
               "       fh2mod <mod> <command> [args ...]\n"
               "       fh2mod --pid <process id> ...   (when more than one game runs)\n"
               "Runs a mod's command in the running Forza Horizon 2 (FH2 Mod Platform 2.8.0 or newer).\n",
               stderr);
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int i = 1;
    std::wstring pid;
    if (i + 1 < argc && std::wstring(argv[i]) == L"--pid") {
        pid = argv[i + 1];
        i += 2;
    }
    if (i >= argc) return usage();
    std::string line;
    if (std::wstring(argv[i]) == L"list") {
        line = "list";
    } else {
        if (i + 1 >= argc) return usage();
        const std::string mod = utf8(argv[i]);
        line = mod.find(' ') != std::string::npos ? "\"" + mod + "\"" : mod;
        for (int a = i + 1; a < argc; ++a) line += " " + utf8(argv[a]);
    }
    if (pid.empty()) {
        const auto pipes = game_pipes();
        if (pipes.empty()) {
            std::fputs("No game with mod commands is running (or its mods add none).\n", stderr);
            return 1;
        }
        if (pipes.size() > 1) {
            std::fputs("More than one game is running; pick one with --pid:", stderr);
            for (const auto& p : pipes) std::fprintf(stderr, " %ls", p.c_str());
            std::fputs("\n", stderr);
            return 1;
        }
        pid = pipes[0];
    }
    const std::wstring name = L"\\\\.\\pipe\\fh2mod-" + pid;
    HANDLE p                = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 50; ++tries) {
        p = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name.c_str(), 200);
    }
    if (p == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "Couldn't reach %ls (error %lu).\n", name.c_str(), GetLastError());
        return 1;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(p, &mode, nullptr, nullptr);
    DWORD w = 0;
    if (!WriteFile(p, line.data(), static_cast<DWORD>(line.size()), &w, nullptr)) {
        std::fprintf(stderr, "Couldn't send the command (error %lu).\n", GetLastError());
        CloseHandle(p);
        return 1;
    }
    std::string reply;
    char buf[4096];
    for (;;) {
        DWORD n       = 0;
        const BOOL ok = ReadFile(p, buf, sizeof buf, &n, nullptr);
        reply.append(buf, n);
        if (ok || GetLastError() != ERROR_MORE_DATA) break;
    }
    CloseHandle(p);
    _setmode(_fileno(stdout), _O_BINARY);   // the reply as it came, for tools reading it
    std::fwrite(reply.data(), 1, reply.size(), stdout);
    return reply.rfind("error: ", 0) == 0 ? 1 : 0;
}
