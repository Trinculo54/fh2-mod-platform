// A mod for the platform test's online phases: made for offline play (FH2ModInfo.offline_only, API 9).
#include "fh2mod.h"

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    FH2ModInfo info{sizeof info, "OfflineMod", "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    info.description  = "Made for offline play.";
    info.offline_only = 1;
    FH2Mod* m = api->register_mod(&info);
    if (!m) return 1;   // the platform turned it away: the game is online
    api->log(m, api->playing_online() ? "running online" : "running offline");
    return 0;
}
