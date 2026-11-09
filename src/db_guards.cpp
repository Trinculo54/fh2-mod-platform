// The database guards (db_guards.hpp), the platform's side: a thread that waits for the game's code, then puts the
// guards in or says why not, and the hooks themselves.
//
// DriveTrainConverter (game+0xB99BE0) turns a car's PowertrainID into the drivetrain's text ("AWD", with the
// "text" parameter), its icon's path (any other parameter) or the icon itself (the other type it can make). All three
// look the Id up in the car database cache's Powertrains table (game+0x26F900) and use the record unchecked
// (they read it at game+0xB99CD3, 0xB99D35 and 0xB99D9E). Each is reached only once the converter has decided it can make the type
// the binding asks for, and that decision is "test al,al; je" twice: at game+0xB99CA0 (a string? no: try the other
// type) and at game+0xB99D74 (the other type? no: give up). Giving up (game+0xB99E37) is the converter's own path for
// a binding it can't serve: it releases its parameter (the AddRef at game+0xB99C8A) and returns the game's empty
// value, AddRef'd for the caller (game+0x160C340, a static object). Nothing else is held there: the converter makes
// its temporaries only after the lookup. So when the Id isn't in the table, both hooks clear al, and the converter
// leaves exactly as it does for a type it can't make; the binding shows nothing, and the car is listed without its
// drivetrain. tests/hook_test.cpp runs the converter natively against the real exe, with the guard in, to show it.
//
// The hook only reads: three pointers and one record pointer, each through safe_copy. It logs once a session, and
// counts the rest. It makes no game calls. When the guards go in, the thread also names the cars the database gives
// a powertrain the table doesn't have (database.cpp reads the file the game reads), so the log has them before any
// menu lists one.
#include "db_guards.hpp"

#include "platform.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh2platform {
namespace {

FH2Mod make_owner() {
    FH2Mod m;
    m.name          = "Database guards";
    m.wname         = L"Database guards";
    m.check_updates = false;
    return m;
}
FH2Mod g_owner = make_owner();   // owns the hooks; never registered
const FH2ModApi* g_api = nullptr;
std::atomic<uintptr_t> g_base{0};
std::atomic<int> g_source{0};          // DbSource, for the log line
constexpr uint32_t kNoId = 0xFFFFFFFFu;
std::atomic<uint32_t> g_first_id{kNoId};   // the PowertrainID it fired for first (set before g_fired counts it)
std::atomic<uint32_t> g_fired{0};          // times a guard fired this session

bool read_memory(uintptr_t at, void* out, std::size_t n) { return at >= 0x10000 && safe_copy(out, reinterpret_cast<const void*>(at), n); }

// On the game's UI thread. al holds the converter's answer to "can I make this type?".
void on_type_check(FH2Context* ctx, void*) {
    if ((ctx->rax & 0xFF) == 0) return;   // it can't anyway: the game goes its own way
    const auto id         = static_cast<uint32_t>(ctx->rbx);
    const Powertrain what = powertrain_lookup(&read_memory, g_base.load(std::memory_order_relaxed), id);
    if (what == Powertrain::known) return;
    ctx->rax &= ~0xFFull;   // "no": the converter returns the game's empty value
    uint32_t none = kNoId;
    g_first_id.compare_exchange_strong(none, id);
    if (g_fired.fetch_add(1) == 0) {
        log_line(g_owner.name, guard_fired_line(id, what, static_cast<DbSource>(g_source.load())));
    }
}

// Every check as the exe on disk has it; the first that doesn't match is logged.
bool code_as_expected() {
    for (std::size_t i = 0; i < kGuardCodeCheckCount; ++i) {
        const GuardCodeCheck& c = kGuardCodeChecks[i];
        uint8_t want[256];
        const std::size_t n = guard_parse_hex(c.hex, want, sizeof want);
        if (!n || !disk_bytes_equal(reinterpret_cast<const void*>(g_base.load() + c.rva), want, n)) {
            char b[256];
            std::snprintf(b, sizeof b, "off: game+0x%X (%s) doesn't match the exe on disk", static_cast<unsigned>(c.rva), c.what);
            log_line(g_owner.name, b);
            return false;
        }
    }
    return true;
}

void start_thread() {
    void* base = g_api->wait_for_code(0xFFFFFFFFu);
    if (!base) return;   // another game, or no game: the platform has said so
    g_base = reinterpret_cast<uintptr_t>(base);
    const DbSource source = db_source(database_facts());
    g_source              = static_cast<int>(source);
    if (source == DbSource::games_own) {
        log_line(g_owner.name, "off: the game reads its own database (no car pack or mod changes it)");
        return;
    }
    if (!disk_image_readable()) {
        log_line(g_owner.name, "off: the game exe couldn't be read from the game folder, so the drivetrain code can't be checked");
        return;
    }
    if (!code_as_expected()) return;
    FH2Hook* h[2] = {
        g_api->hook_instruction(&g_owner, "DriveTrainConverter: the string check (database guard)",
                                reinterpret_cast<void*>(g_base.load() + kRvaStringCheck), &on_type_check, nullptr),
        g_api->hook_instruction(&g_owner, "DriveTrainConverter: the other type's check (database guard)",
                                reinterpret_cast<void*>(g_base.load() + kRvaOtherCheck), &on_type_check, nullptr),
    };
    bool all = h[0] && h[1];
    for (int i = 0; i < 2 && all; ++i) all = g_api->enable_hooks(&g_owner, h[i]) != 0;
    if (!all) {
        for (FH2Hook* x : h)
            if (x) g_api->disable_hooks(&g_owner, x);
        log_line(g_owner.name, "off: the drivetrain code couldn't be hooked (see the lines above)");
        return;
    }
    log_line(g_owner.name, std::string("on (") + db_source_text(source) +
                               "): a car whose powertrain isn't in the game's Powertrains table is shown without its "
                               "drivetrain instead of stopping the game");
    // Which cars those are, from the database itself, so the log names them before any menu lists one.
    const std::vector<std::string> cars = cars_without_powertrain();
    if (cars.empty()) return;
    std::string list;
    for (const std::string& c : cars) list += (list.empty() ? "" : ", ") + c;
    log_line(g_owner.name, "these cars' powertrains (in brackets) aren't in its Powertrains table, so the Autoshow and garages "
                           "list them without their drivetrain: " + list + (cars.size() >= 20 ? ", and maybe more" : "") +
                           ". The likely cause is a car pack or a mod that changes the game database.");
}

} // namespace

void db_guards_start(const FH2ModApi* api) {
    static std::once_flag once;
    if (!api) return;
    std::call_once(once, [api] {
        g_api = api;
        std::thread(start_thread).detach();
    });
}

bool db_guard_fired(uint32_t* powertrain, uint32_t* times) {
    const uint32_t n = g_fired.load();
    if (!n) return false;
    if (powertrain) *powertrain = g_first_id.load();
    if (times) *times = n;
    return true;
}

} // namespace fh2platform
