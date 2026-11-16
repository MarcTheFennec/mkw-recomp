// The game files setup the start screen runs (ui/launcher/launcher_setup.cpp): tools/prepare/prepare.py in a child process, with
// the package's Python (setup/ next to the executable) or, in a source checkout, the repository's.
#pragma once

enum { SETUP_STEPS = 8 };   // prepare.py STEPS: check, exefs, romfs, generate, compile, shaders, warm, cleanup

typedef struct {
    int running;
    int step;                // the step being run, 0..SETUP_STEPS-1; -1 none yet
    int done[SETUP_STEPS];   // 1 run, 2 skipped (already done by an earlier setup)
    long progress, total;    // of the step being run; total 0: no count
    int finished;            // 1 done, -1 failed or cancelled
    char code[16];           // why, when it failed: prepare.py's error code (its header), or "stopped", "cancelled",
                             // "no_setup" (the tools missing) and "" for a message of SDL's
    char error[512];         // the detail: a folder, a build id, a tool's English message...
} SetupState;

int setup_available(const char *exe_dir);   // the Python and the script are there
// The package's Python and tools/<rel> (import.py), for pages that run scripts like the setup does.
int setup_tool(const char *exe_dir, const char *rel, char *python, char *script, size_t cap);
// source NULL: the last setup's copy (after an update: only what changed runs again). link_romfs: the player's pick
// from the files page's cards (0 copy, 1 link), or < 0 for neither and prepare.py's own record of both.
void setup_start(const char *exe_dir, const char *game_dir, const char *source, int link_romfs);
void setup_cancel(void);
SetupState setup_state(void);                // a copy, any time
