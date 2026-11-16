// The Tools page's jobs (ui/launcher/launcher_tools.cpp): a script of tools/prepare (saves.py, steam.py) run with the
// package's Python in a child process, one at a time, off the UI thread; its last output line is kept ("done ..." or
// "error <code> <detail>", the scripts' own protocol).
#pragma once

enum ToolPhase { TOOL_IDLE, TOOL_RUNNING, TOOL_DONE, TOOL_FAILED };

typedef struct {
    int phase;       // ToolPhase
    int job;         // the caller's id of what ran: which row shows the result
    char line[600];  // the script's last line, without "done " / "error "
} ToolState;

// script: under tools/ ("prepare/saves.py"); args: after it, NULL-terminated, at most 12. 0 when a job already runs or
// the package has no Python (a development build).
int tool_start(const char *exe_dir, int job, const char *script, const char *const *args);
ToolState tool_state(void);   // a copy, any time
