// Discord Rich Presence (presence.c): the player's Discord profile shows the game while it runs.
#pragma once

void presence_start(void);                    // boot.c: starts the presence thread once (it reads MKW_DISCORD live)
void presence_file_opened(const char *path);  // fs_host.c: a RomFS or save file opened; the game's phase from its name
// --probe-discord (probes/discord_probe.c)
int presence_connected(void);                 // Discord answered the handshake and the connection holds
const char *presence_describe(void);          // what the profile says now ("Racing / Airship Fortress"); one thread
extern const char *const presence_probe_steps[];   // sample paths for presence_file_opened, NULL-terminated
