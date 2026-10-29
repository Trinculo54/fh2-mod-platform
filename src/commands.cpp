// Commands (API 9, platform 2.8.0). Mods register named commands, and other programs run them
// while the game runs: fh2mod.exe (tools/fh2mod.cpp), or anything that writes a line to the
// named pipe \\.\pipe\fh2mod-<process id>. The pipe only starts once a mod adds a command, takes
// local clients only (PIPE_REJECT_REMOTE_CLIENTS) of the same Windows user (a DACL with just the
// user's SID; without one, there's no pipe), and serves one request at a time on its own thread.
// A request is one line of UTF-8: "list", or "<mod> <command> [args]" (a mod name with spaces
// in double quotes); the reply is text, and the connection then closes.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace fh2platform {

namespace {

struct Command {
    FH2Mod* mod;
    std::string name, help;
    FH2CommandFn fn;
    void* user;
};
std::mutex g_mu;
std::deque<Command> g_commands;
bool g_started = false;
constexpr DWORD kRequestMax = 4096;
constexpr std::size_t kReplyMax = 16384;

bool name_is(const char* k) {
    if (!k || !*k || std::strlen(k) > 40) return false;
    for (const char* c = k; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
    return true;
}

bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool call_command(FH2CommandFn fn, void* user, const char* name, const char* args, char* reply, std::size_t size) {
    __try {
        fn(user, name, args, reply, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One request, one reply.
std::string handle(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line == "list" || line.empty()) {
        std::string out;
        std::scoped_lock lk{g_mu};
        for (const Command& c : g_commands)
            out += (c.mod->name.find(' ') != std::string::npos ? "\"" + c.mod->name + "\"" : c.mod->name) + " " + c.name +
                   (c.help.empty() ? "" : "  " + c.help) + "\n";
        return out.empty() ? "no commands\n" : out;
    }
    std::size_t at = 0;
    std::string mod;
    if (line[0] == '"') {
        const std::size_t end = line.find('"', 1);
        if (end == std::string::npos) return "error: a mod name in quotes needs its closing quote\n";
        mod = line.substr(1, end - 1);
        at  = end + 1;
    } else {
        at  = line.find(' ');
        mod = line.substr(0, at);
    }
    while (at < line.size() && line[at] == ' ') ++at;
    const std::size_t end  = line.find(' ', at);
    const std::string name = at < line.size() ? line.substr(at, end == std::string::npos ? std::string::npos : end - at) : "";
    std::string args       = end == std::string::npos ? "" : line.substr(end + 1);
    while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    FH2CommandFn fn = nullptr;
    void* user      = nullptr;
    FH2Mod* owner   = nullptr;
    bool known_mod  = false;
    std::string cname;
    {
        std::scoped_lock lk{g_mu};
        for (const Command& c : g_commands) {
            if (!iequal(c.mod->name, mod)) continue;
            known_mod = true;
            if (iequal(c.name, name)) {
                fn    = c.fn;
                user  = c.user;
                owner = c.mod;
                cname = c.name;
            }
        }
    }
    if (!known_mod) return "error: no mod called \"" + mod + "\" has commands (fh2mod list shows them)\n";
    if (!fn) return "error: " + mod + " has no command called \"" + name + "\" (fh2mod list shows them)\n";
    static char reply[kReplyMax];
    reply[0] = '\0';
    log_line(owner->name, "command: " + cname + (args.empty() ? "" : " " + args));
    if (!call_command(fn, user, cname.c_str(), args.c_str(), reply, sizeof reply)) {
        log_line(owner->name, "crashed in its command " + cname);
        return "error: " + owner->name + "'s command " + cname + " crashed\n";
    }
    reply[sizeof reply - 1] = '\0';
    std::string out = reply;
    if (out.empty() || out.back() != '\n') out += "\n";
    return out;
}

using OpenProcessTokenFn  = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
using GetTokenInfoFn      = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
using SidToStringFn       = BOOL(WINAPI*)(PSID, LPWSTR*);
using SddlToDescriptorFn  = BOOL(WINAPI*)(LPCWSTR, DWORD, PSECURITY_DESCRIPTOR*, PULONG);

// A security descriptor that lets only this Windows user in (full access), or null. advapi32
// is loaded here, from System32, not linked: see CMakeLists.txt.
PSECURITY_DESCRIPTOR user_only() {
    HMODULE a = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!a) return nullptr;
    auto open_token = reinterpret_cast<OpenProcessTokenFn>(reinterpret_cast<void*>(GetProcAddress(a, "OpenProcessToken")));
    auto token_info = reinterpret_cast<GetTokenInfoFn>(reinterpret_cast<void*>(GetProcAddress(a, "GetTokenInformation")));
    auto sid_string = reinterpret_cast<SidToStringFn>(reinterpret_cast<void*>(GetProcAddress(a, "ConvertSidToStringSidW")));
    auto sddl       = reinterpret_cast<SddlToDescriptorFn>(
        reinterpret_cast<void*>(GetProcAddress(a, "ConvertStringSecurityDescriptorToSecurityDescriptorW")));
    if (!open_token || !token_info || !sid_string || !sddl) return nullptr;
    HANDLE token = nullptr;
    if (!open_token(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    alignas(8) unsigned char buf[512];
    DWORD got   = 0;
    const BOOL ok = token_info(token, TokenUser, buf, sizeof buf, &got);
    CloseHandle(token);
    if (!ok) return nullptr;
    LPWSTR sid = nullptr;
    if (!sid_string(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &sid)) return nullptr;
    const std::wstring text = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    return sddl(text.c_str(), 1 /* SDDL_REVISION_1 */, &sd, nullptr) ? sd : nullptr;
}

DWORD WINAPI serve(LPVOID sd) {
    const std::wstring name = L"\\\\.\\pipe\\fh2mod-" + std::to_wstring(GetCurrentProcessId());
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    for (;;) {
        HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, kReplyMax, kRequestMax, 0, &sa);
        if (p == INVALID_HANDLE_VALUE) {
            plog("commands: couldn't make the pipe (error " + std::to_string(GetLastError()) + "); commands are off");
            return 0;
        }
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char req[kRequestMax];
            DWORD n = 0;
            if (ReadFile(p, req, sizeof req - 1, &n, nullptr) || GetLastError() == ERROR_MORE_DATA) {
                const std::string reply = handle(std::string(req, n));
                DWORD w                 = 0;
                WriteFile(p, reply.data(), static_cast<DWORD>(reply.size()), &w, nullptr);
                FlushFileBuffers(p);
            }
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}

void start() {
    PSECURITY_DESCRIPTOR sd = user_only();
    if (!sd) {
        plog("commands: couldn't make a pipe only this Windows user can reach; commands are off");
        return;
    }
    const std::string name = "\\\\.\\pipe\\fh2mod-" + std::to_string(GetCurrentProcessId());
    if (HANDLE t = CreateThread(nullptr, 0, serve, sd, 0, nullptr)) {
        CloseHandle(t);
        plog("commands: other programs on this PC (this Windows user's) can run mods' commands through " + name +
             ", or with fh2mod.exe");
        set_session_pipe(name);
    }
}

} // namespace

int command_add(FH2Mod* m, const char* name, const char* help, FH2CommandFn fn, void* user) {
    if (!m) return 0;
    if (!name_is(name)) {
        log_line(m->name, std::string("add_command: \"") + (name ? name : "") + "\" isn't a usable name (letters, digits, _)");
        return 0;
    }
    if (!fn) {
        log_line(m->name, std::string("add_command: ") + name + " needs a function");
        return 0;
    }
    bool first = false;
    {
        std::scoped_lock lk{g_mu};
        for (const Command& c : g_commands)
            if (c.mod == m && iequal(c.name, name)) {
                log_line(m->name, std::string("add_command: there's already a command called ") + name);
                return 0;
            }
        g_commands.push_back({m, name, help ? help : "", fn, user});
        first     = !g_started;
        g_started = true;
    }
    log_line(m->name, std::string("command: ") + name + (help && *help ? " (" + std::string(help) + ")" : ""));
    if (first) start();
    return 1;
}

} // namespace fh2platform
