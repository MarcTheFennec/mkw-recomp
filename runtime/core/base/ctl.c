// MKW_CTL=<fifo>: commands to a running game, one per line, so a screen is inspected where it is instead of a new run
// replaying the boot and the menus to reach its frame again (60-90 s each). The FIFO is made if missing; answers are
// "ctl:" lines in the log. tools/run/ctl.sh writes a command and waits for its answer.
//   shot PATH            the next presented frame, full size, as PNG (save_shot)
//   screen PATH          the next swapchain image as PNG: what the player sees, with the menu, notices and overlays
//                        (sRGB at paper white on an HDR display; gfx_present.c save_capture)
//   dump DIR [ubo|lut|log N]   the next frame as MKW_DUMP writes it (vk_dump_rearm; ubo: MKW_DUMP_UBO; lut: only 3D
//                        volumes; log N: the N frames before it logged to passes.txt too, as MKW_DUMP_LOG)
//   flight               a flight dump around now (MKW_FLIGHT=1), as F8
//   press <ticks> <button> [hold]  /  press <ticks> LSTICK|RSTICK x y [hold]   input, <ticks> 60 Hz ticks from now
//   tex ADDR             textures whose storage or pool-bound Vulkan image covers ADDR (0: every image larger than its storage)
//   mem ADDR SIZE PATH   SIZE bytes of guest memory at ADDR to PATH (what a CPU-written texture holds)
//   frame                the present count
//   trim [MiB]           frees that much (1024) as an allocation failure would: released textures, idle mirrored ones
//                        (vk_mem_relieve), with the vram line before and after
//   vram                 the "mem: vram" line now: what holds device-local memory (vk_mem_log.c, MKW_VRAM_LOG)
//   focus 0|1            the window lost (0) or took (1) the focus, as the window system reports it
//   seq                  the game's active sequence nodes (MKW_SEQ_WATCH, seqwatch.c)
//   capture DIR [N]      the next N presented frames (2) as a capture for mkw --replay (runtime/gfx/cap; MKW_CAPTURE=1)
//   pause | step [N] | resume   the game stops on its next frame, goes N frames on (1) and stops, runs again (freeze.c)
//   set KEY VALUE        a live setting as the menu sets it (cfg_set, MKW_ prefixed), not saved to mkw.ini
//   drawstacks [N]       the draws of the next N frames (1) counted by guest call stack and depth target (nvn_dstk.c)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cap.h"
#include "flight.h"
#include "gfx_wsys.h"
#include "hacks.h"
#include "host.h"
#include "input.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

void ctl_run(char *line) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    char cmd[16], arg[400] = "";
    if (sscanf(line, "%15s %399[^\n]", cmd, arg) < 1) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!strcmp(cmd, "shot") && *arg) {
        rt_shot_request(arg);
        rt_log("ctl: shot %s queued at frame %llu", arg, (unsigned long long)frame);
    } else if (!strcmp(cmd, "set") && *arg) {   // a live setting, as the menu sets it (not saved to mkw.ini)
        char *sp = strchr(arg, ' ');
        if (sp) *sp = 0;
        cfg_set(arg, sp ? sp + 1 : "");
        rt_log("ctl: set %s = %s at frame %llu", arg, sp ? sp + 1 : "", (unsigned long long)frame);
    } else if (!strcmp(cmd, "screen") && *arg) {
        vk_present_screen_shot(arg);
        rt_log("ctl: screen %s queued at frame %llu", arg, (unsigned long long)frame);
    } else if (!strcmp(cmd, "dump") && *arg) {
        char d[400];
        int ubo = 0, luts = 0, logn = 0;
        char *sp = strchr(arg, ' ');
        if (sp) *sp = 0, ubo = !strcmp(sp + 1, "ubo"), luts = !strcmp(sp + 1, "lut");
        if (sp && !strncmp(sp + 1, "log ", 4)) logn = atoi(sp + 5);
        snprintf(d, sizeof d, "%s", arg);
        rt_log(vk_dump_rearm(d, ubo, luts, logn) ? "ctl: dump %s queued at frame %llu" : "ctl: dump %s refused: one is pending (frame %llu)",
               d, (unsigned long long)frame);
    } else if (!strcmp(cmd, "flight")) {
        if (!flight_on) rt_log("ctl: flight refused: MKW_FLIGHT is off");
        else flight_request(), rt_log("ctl: flight queued at frame %llu", (unsigned long long)frame);
    } else if (!strcmp(cmd, "press")) {
        rt_log(input_script_inject(arg) ? "ctl: press %s queued at frame %llu" : "ctl: press %s: bad line (frame %llu)", arg,
               (unsigned long long)frame);
    } else if (!strcmp(cmd, "tex") && *arg) {
        vk_texture_report(strtoull(arg, NULL, 0));
    } else if (!strcmp(cmd, "mem") && *arg) {
        unsigned long long a = 0, n = 0;
        char path[400];
        FILE *o;
        if (sscanf(arg, "%llx %llx %399s", &a, &n, path) != 3 || !(o = fopen(path, "wb"))) {
            rt_log("ctl: mem %s: needs ADDR SIZE PATH (hex) and a writable path", arg);
        } else {
            mem_commit_range(a, n);   // a large fwrite hands the buffer to the kernel (Windows' lazy commit)
            size_t w = fwrite((const void *)(uintptr_t)a, 1, (size_t)n, o);   // guest addresses are host addresses
            fclose(o);
            rt_log("ctl: mem %#llx: %zu bytes to %s", a, w, path);
        }
    } else if (!strcmp(cmd, "capture") && *arg) {
        char dir[400];
        int frames = 2;
        if (sscanf(arg, "%399s %d", dir, &frames) < 1) frames = 2;
        if (!cap_request(dir, frames)) rt_log("ctl: capture refused: the run needs MKW_CAPTURE=1");
        else rt_log("ctl: capture %s (%d frames) queued at frame %llu", dir, frames, (unsigned long long)frame);
    } else if (!strcmp(cmd, "drawstacks")) {
        nvn_dstk_request(*arg ? atoi(arg) : 1);
    } else if (!strcmp(cmd, "seq")) {
        seqwatch_log_active();
    } else if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume")) {
        freeze_set(cmd[0] == 'p', 0);
    } else if (!strcmp(cmd, "step")) {
        freeze_set(1, *arg ? atoll(arg) : 1);
    } else if (!strcmp(cmd, "vram")) {
        vram_log_now();   // vk_mem_log.c: what holds device-local memory, as MKW_VRAM_LOG's periodic line
        rt_log("ctl: vram at frame %llu", (unsigned long long)frame);
    } else if (!strcmp(cmd, "trim")) {
        // vk_mem_relieve, as when an allocation fails: released textures, then idle mirrored ones, then the GPU waited
        // for and what it held freed; the vram line before and after shows what went back
        uint64_t mib = *arg ? strtoull(arg, NULL, 10) : 1024;
        vram_log_now();
        vk_mem_relieve(mib << 20, "ctl trim");
        vram_log_now();
        rt_log("ctl: trim %llu MiB at frame %llu", (unsigned long long)mib, (unsigned long long)frame);
    } else if (!strcmp(cmd, "frame")) {
        rt_log("ctl: frame %llu", (unsigned long long)frame);
    } else if (!strcmp(cmd, "focus") && (*arg == '0' || *arg == '1')) {
        gfx_window_focus_changed(*arg == '1');
        rt_log("ctl: focus %c at frame %llu", *arg, (unsigned long long)frame);
    } else {
        rt_log("ctl: unknown command: %s", line);
    }
}

#ifndef _WIN32
#include <sys/stat.h>

static void ctl_main(void *arg) {
    const char *path = arg;
    // Read and write: the FIFO never reaches end of file when a writer closes it, so fgets just waits for the next one.
    FILE *f = fopen(path, "r+");
    if (!f) { rt_log("ctl: cannot open %s (%s)", path, strerror(errno)); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) ctl_run(line);
    rt_log("ctl: %s closed", path);
}

void ctl_init(void) {
    const char *path = cfg_get("MKW_CTL");
    if (!path || !*path) return;
    struct stat st;
    if (stat(path, &st) ? mkfifo(path, 0600) != 0 : !S_ISFIFO(st.st_mode)) {
        rt_log("ctl: %s is not a FIFO and cannot be made one, off", path);
        return;
    }
    if (!plat_thread_spawn(ctl_main, (void *)path, "ctl", 0)) rt_log("ctl: cannot start its thread");
    else rt_log("ctl: reading commands from %s", path);
}
#else
#include <windows.h>

// (a remote run writes one, over SSH, and reads the answer from the log).
static void ctl_main(void *arg) {
    const char *path = arg;
    for (;;) {
        HANDLE p = CreateNamedPipeA(path, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0, 4096, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) { rt_log("ctl: cannot make %s (error %lu)", path, GetLastError()); return; }
        if (ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[512];
            size_t n = 0;
            DWORD got;
            while (ReadFile(p, buf + n, (DWORD)(sizeof buf - 1 - n), &got, NULL) && got) {
                buf[n += got] = 0;
                char *s = buf, *nl;
                while ((nl = strchr(s, '\n'))) *nl = 0, ctl_run(s), s = nl + 1;
                n = strlen(s);
                memmove(buf, s, n + 1);
                if (n == sizeof buf - 1) n = 0;   // a line longer than the buffer: dropped
            }
            if (n) ctl_run(buf);
        }
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}

void ctl_init(void) {
    const char *v = cfg_get("MKW_CTL");
    if (!v || !*v) return;
    static char path[300];
    const char *b = strrchr(v, '\\'), *b2 = strrchr(v, '/');
    if (b2 > b) b = b2;
    snprintf(path, sizeof path, "\\\\.\\pipe\\%s", b ? b + 1 : v);
    if (!plat_thread_spawn(ctl_main, path, "ctl", 0)) rt_log("ctl: cannot start its thread");
    else rt_log("ctl: reading commands from %s", path);
}
#endif
