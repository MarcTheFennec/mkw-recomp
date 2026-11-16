# runtime/core/presence

Discord Rich Presence: while the game runs, the player's Discord profile shows it, the time played and what the player
is doing, in English ("Playing Mario Kart World", "Time Trial", "Moo Moo Meadows"). Off by default (the player's Discord friends see it), `MKW_DISCORD` (Settings > General) turns it on live; `MKW_DISCORD_APP` names another Discord
application. It talks only to the Discord desktop client on the same machine, through its local pipe, with frames
written here from Discord's own description of the protocol: no SDK, no library, nothing over the network. Without
Discord it retries every 15 s and never makes the game wait. `--probe-discord` checks it without the game.

| File | What it is |
|---|---|
| `presence.c`, `presence.h` | The presence thread (connection, handshake, `SET_ACTIVITY`), and the game's phase from the files it opens (`fs_host.c`) |
| `course_names.c`, `course_names.h` | The game's own English course names, read once from the player's texts (`Mals/USen/MsgMain.msarc`) |
