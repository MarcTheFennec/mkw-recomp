// --probe-discord: Discord Rich Presence (core/presence/presence.c) without the game. Starts the presence thread,
// waits for it to reach the Discord desktop client, then plays the phases the game's file opens set (the same paths
// presence_file_opened sees in a game), a few seconds each, so each one can be checked on the profile. Fails when no
// Discord answers within 20 s; needs Discord running on this machine (MKW_DISCORD_APP: another application id).
#include <stdio.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "presence.h"

#define HOLD_MS 6000   // each phase: Discord shows an update within about 2 s, and the eye needs the rest

int probe_discord(void) {
    cfg_set("MKW_DISCORD", "1");
    presence_start();
    uint64_t t0 = plat_now_ms();
    while (!presence_connected() && plat_now_ms() - t0 < 20000) plat_sleep_ms(100);
    if (!presence_connected()) {
        printf("FAIL discord: no Discord answered in 20 s (is the desktop client running?)\n");
        return 1;
    }
    printf("ok   discord: connected after %llu ms\n", (unsigned long long)(plat_now_ms() - t0));
    for (int i = 0; presence_probe_steps[i]; i++) {
        presence_file_opened(presence_probe_steps[i]);
        printf("     discord: %s -> %s\n", presence_probe_steps[i], presence_describe());
        plat_sleep_ms(HOLD_MS);
        if (!presence_connected()) {
            printf("FAIL discord: the connection dropped\n");
            return 1;
        }
    }
    cfg_set("MKW_DISCORD", "0");   // the thread clears the activity and lets go
    plat_sleep_ms(1500);
    printf("ok   discord: cleared\n");
    return 0;
}
