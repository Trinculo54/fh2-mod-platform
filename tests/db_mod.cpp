// A mod for the database test (tests/database_test.cpp). It's copied into
// fh2-mods under the name of the mod it plays, and that name picks its SQL:
//   NitroTest.dll   every car can backfire
//   CountTest.dll   adds 1 to AI driver 1's XP, so changes made twice show
//   BadSqlTest.dll  SQL that fails
//   CarIdTest.dll   adds car 3000 itself (a copy of car 441), in the range car packs' cars are given
//   CarIdLate.dll   adds engine 3003 itself (car pack cars' engines have their car's Id)
//   ZipTest.dll     replaces zipmanifest.xml with set_file (refused while car packs' zips are in it), no SQL
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
                      : g_name == "CarIdTest"
                          ? "CREATE TEMP TABLE hc AS SELECT * FROM Data_Car WHERE Id = 441;"
                            "UPDATE hc SET Id = 3000, MediaName = 'HAND_Test01';"
                            "INSERT INTO Data_Car SELECT * FROM hc; DROP TABLE hc;"
                      : g_name == "CarIdLate"
                          ? "CREATE TEMP TABLE he AS SELECT * FROM Data_Engine ORDER BY EngineID LIMIT 1;"
                            "UPDATE he SET EngineID = 3003;"
                            "INSERT INTO Data_Engine SELECT * FROM he; DROP TABLE he;"
                          : "UPDATE NoSuchTable SET X = 1;";
    FH2ModInfo info{sizeof info, g_name.c_str(), "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    FH2Mod* m = api->register_mod(&info);
    if (g_name == "ZipTest") {
        const char manifest[] = "<ZipFiles>\r\n</ZipFiles>\r\n";
        api->log(m, api->set_file(m, "zipmanifest.xml", manifest, sizeof manifest - 1) ? "zipmanifest.xml replaced" : "zipmanifest.xml refused");
        return 0;
    }
    api->add_sql(m, sql);
    return 0;
}
