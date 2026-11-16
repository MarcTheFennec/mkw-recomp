# runtime/core

Boot, modules, kernel, guest memory, settings and diagnostics. Every subfolder with headers is on the include path
(`tools/build/build.py`): `#include` goes by name.

| Folder | Contents |
|---|---|
| [boot/](boot/README.md) | Entry point, boot sequence, game code tables, module loader, patches, game clock |
| [kernel/](kernel/README.md) | Horizon kernel: handles, waits, threads, virtual cores, fibers, SVCs |
| [memory/](memory/README.md) | Guest address space, write tracking, read traps (`MKW_EARLY_READ`) |
| [base/](base/README.md) | `base/host.h`, settings (`cfg`), `MKW_CTL`, system info, SHA-256, UTF-16 |
| [hacks/](hacks/README.md) | Shortcuts the console does not have: turbo, skip movies, development helpers |
| [presence/](presence/README.md) | Discord Rich Presence: the Discord profile shows the game and what the player does (`MKW_DISCORD`) |
| [diag/](diag/README.md) | Flight recorder, counters, profiling, RenderDoc, `MKW_SEQ_WATCH`, `MKW_IMPORT_CENSUS` |
