// A mod for the database test (tests/database_test.cpp). It's copied into
// fh2-mods under the name of the mod it plays, and that name picks its SQL:
//   NitroTest.dll   every car can backfire
//   CountTest.dll   adds 1 to AI driver 1's XP, so changes made twice show
//   BadSqlTest.dll  SQL that fails
#include "fh2mod.h"
#include <windows.h>

#include <string>

static std::string g_name;

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&fh2mod_init), &self);
    char path[MAX_PATH]{};
    GetModuleFileNameA(self, path, MAX_PATH);
    g_name = path;
    g_name = g_name.substr(g_name.find_last_of("\\/") + 1);
    g_name = g_name.substr(0, g_name.rfind('.'));
    const char* sql = g_name == "NitroTest"   ? "UPDATE BackfireLevels SET BackfireLevel = 3;"
                      : g_name == "CountTest" ? "UPDATE AIPlayers SET XP = XP + 1 WHERE Id = 1;"
                                              : "UPDATE NoSuchTable SET X = 1;";
    FH2ModInfo info{sizeof info, g_name.c_str(), "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    FH2Mod* m = api->register_mod(&info);
    api->add_sql(m, sql);
    return 0;
}
