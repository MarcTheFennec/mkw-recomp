// The Tools page's jobs (launcher_tools.h). A thread starts the script and reads all its output when it ends
// (SDL_ReadProcess): the scripts print one line, so there is nothing to stream, and the UI thread never waits.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "ui/launcher/launcher_setup.h"
#include "ui/launcher/launcher_tools.h"

extern "C" {
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

enum { MAX_ARGS = 16 };

static SDL_Mutex *g_lock;
static ToolState g_state;
static struct {
    char store[MAX_ARGS][1200];   // the command line, copied: the caller's strings may be gone when the thread runs
    const char *argv[MAX_ARGS + 1];
} g_cmd;

static void finish(int phase, const char *line) {
    SDL_LockMutex(g_lock);
    g_state.phase = phase;
    snprintf(g_state.line, sizeof g_state.line, "%s", line);
    SDL_UnlockMutex(g_lock);
}

static int SDLCALL run(void *) {
    SDL_Process *p = SDL_CreateProcess(g_cmd.argv, true);
    if (!p) {
        finish(TOOL_FAILED, SDL_GetError());
        return 0;
    }
    size_t n = 0;
    int code = -1;
    char *out = (char *)SDL_ReadProcess(p, &n, &code);
    SDL_DestroyProcess(p);
    // the last non-empty line: "done <what>" or "error <code> <detail>"
    char none[1] = "", *line = out ? out : none;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    if (char *nl = strrchr(line, '\n')) line = nl + 1;
    line[strcspn(line, "\r")] = 0;
    // only the scripts' own words: "done" alone or "done ...", so a traceback mentioning one does not read as a result
    bool is_done = !strncmp(line, "done", 4) && (!line[4] || line[4] == ' ');
    bool is_err = !strncmp(line, "error", 5) && (!line[5] || line[5] == ' ');
    bool ok = code == 0 && is_done;
    const char *rest = is_done ? line + 4 : is_err ? line + 5 : line;
    while (*rest == ' ') rest++;
    rt_log("tools: %s %s: %s", g_cmd.argv[2], ok ? "done" : "failed", rest);
    finish(ok ? TOOL_DONE : TOOL_FAILED, rest);
    SDL_free(out);
    return 0;
}

int tool_start(const char *exe_dir, int job, const char *script, const char *const *args) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (tool_state().phase == TOOL_RUNNING) return 0;
    char python[1200], path[1200];
    if (!setup_tool(exe_dir, script, python, path, sizeof python)) return 0;
    int n = 0;
    const char *head[] = {python, "-I", path};
    for (const char *a : head) snprintf(g_cmd.store[n++], sizeof g_cmd.store[0], "%s", a);
    for (; args && *args && n < MAX_ARGS; args++) snprintf(g_cmd.store[n++], sizeof g_cmd.store[0], "%s", *args);
    for (int i = 0; i < n; i++) g_cmd.argv[i] = g_cmd.store[i];
    g_cmd.argv[n] = nullptr;
    SDL_LockMutex(g_lock);
    g_state = {TOOL_RUNNING, job, ""};
    SDL_UnlockMutex(g_lock);
    SDL_Thread *t = SDL_CreateThread(run, "tools", nullptr);
    if (!t) {
        finish(TOOL_FAILED, SDL_GetError());
        return 0;
    }
    SDL_DetachThread(t);
    return 1;
}

ToolState tool_state(void) {
    if (!g_lock) return g_state;
    SDL_LockMutex(g_lock);
    ToolState s = g_state;
    SDL_UnlockMutex(g_lock);
    return s;
}
