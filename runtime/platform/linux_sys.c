// Linux implementation of platform.h, process, desktop, libraries, machine load and the desktop shortcut (the parts: linux_int.h).
#ifndef _GNU_SOURCE   // build.py passes it on Linux
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL_clipboard.h>

#include "platform.h"
#include "linux_int.h"

uintptr_t plat_exe_base(void) {
    Dl_info d;
    return dladdr((void *)plat_exe_base, &d) ? (uintptr_t)d.dli_fbase : 0;
}

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    Dl_info d;
    if (!dladdr((void *)addr, &d) || !d.dli_fname) return 0;
    const char *n = strrchr(d.dli_fname, '/');
    *base = (uintptr_t)d.dli_fbase;
    return snprintf(name, cap, "%s", n ? n + 1 : d.dli_fname) < (int)cap;
}

int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = 0;
    return 1;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// Cached per thread: every recursive mutex takes it at each lock (the texture lock, once per texture binding of a draw)
// and gettid is a system call each time.
uint32_t plat_thread_id(void) {
    static _Thread_local uint32_t id;
    if (!id) id = (uint32_t)syscall(SYS_gettid);
    return id;
}

// /etc/localtime is a link to .../zoneinfo/<name>.
int plat_time_zone(char *out, size_t cap) {
    char link[256];
    ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
    if (n <= 0) return 0;
    link[n] = 0;
    const char *z = strstr(link, "zoneinfo/");
    if (!z || strlen(z + 9) >= cap) return 0;
    strcpy(out, z + 9);
    return 1;
}

// The first set variable of names ("ru_RU.UTF-8") as a BCP-47 tag ("ru-RU") into tag; "" for C and POSIX.
static void locale_tag(const char *const *names, int n, char *tag, size_t cap) {
    const char *v = NULL;
    for (int i = 0; i < n && !(v && *v); i++) v = getenv(names[i]);
    size_t k = 0;
    if (v && strcmp(v, "C") && strncmp(v, "C.", 2) && strcmp(v, "POSIX"))
        for (; v[k] && v[k] != '.' && v[k] != '@' && k < cap - 1; k++) tag[k] = v[k] == '_' ? '-' : v[k];
    tag[k] = 0;
}

const char *plat_ui_language(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_TIME", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

// Noto CJK as the common distributions and SteamOS install it; one collection covers every CJK language.
int plat_font_path(const char *lang, char *out, size_t cap) {
    static const char *files[] = {
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    };
    // Thai is not in the CJK collection: Noto Sans Thai (the variable font or the static one), then Tahoma-compatible
    static const char *thai[] = {
        "/usr/share/fonts/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansThai[wdth,wght].ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansThai-Regular.otf",
    };
    if (!strcmp(lang, "th")) {
        for (size_t i = 0; i < sizeof thai / sizeof *thai; i++)
            if (plat_file_exists(thai[i])) return snprintf(out, cap, "%s", thai[i]) < (int)cap;
        return 0;
    }
    if (strcmp(lang, "ja") && strcmp(lang, "ko") && strcmp(lang, "zh-Hans") && strcmp(lang, "zh-Hant")) return 0;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++)
        if (plat_file_exists(files[i])) return snprintf(out, cap, "%s", files[i]) < (int)cap;
    return 0;
}

// PRETTY_NAME of /etc/os-release and the kernel release: "SteamOS (Linux 6.16.12-valve)".
int plat_os_version(char *out, size_t cap) {
    char name[128] = "Linux";
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12, *e;
                if (*v == '"') v++;
                if ((e = strpbrk(v, "\"\n"))) *e = 0;
                snprintf(name, sizeof name, "%s", v);
                break;
            }
        fclose(f);
    }
    struct utsname u;
    if (uname(&u)) return snprintf(out, cap, "%s", name) < (int)cap;
    return snprintf(out, cap, "%s (Linux %s)", name, u.release) < (int)cap;
}

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    if (fstat(2, &st) || !S_ISREG(st.st_mode)) return 0;
    ssize_t n = readlink("/proc/self/fd/2", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = 0;
    return 1;
}

// ---- process

void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

int plat_setenv(const char *name, const char *value) {
    return (value && *value ? setenv(name, value, 1) : unsetenv(name)) == 0;   // empty removes, as _putenv_s on Windows
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

int plat_process_alive(uint32_t pid) { return kill((pid_t)pid, 0) == 0 || errno == EPERM; }

uint64_t plat_process_cpu_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// Field 22 of /proc/self/stat is the start time in clock ticks after boot; CLOCK_BOOTTIME is now after boot.
uint64_t plat_process_uptime_ns(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');   // the command name may hold spaces
    unsigned long long start = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                     &start) != 1)
        return 0;
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
    uint64_t at = start * (1000000000u / (uint64_t)sysconf(_SC_CLK_TCK));
    return now > at ? now - at : 0;
}

uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_r(&t, out);
}

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
    tzset();
    return timezone;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    // the arguments, NUL-separated, from /proc/self/cmdline
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    char exe[PATH_MAX];
    if (!argc || !plat_exe_path(exe, sizeof exe)) return -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    setenv(env_name, env_value, 1);
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    unsetenv(env_name);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) {
        close(p[0]);
        return -1;
    }
    size_t len = 0;
    char sink[512];
    ssize_t k;
    while (len + 1 < cap && (k = read(p[0], out + len, cap - 1 - len)) != 0)
        if (k > 0) len += (size_t)k;
        else if (errno != EINTR) break;
    if (cap) out[len] = 0;
    while ((k = read(p[0], sink, sizeof sink)) > 0 || (k < 0 && errno == EINTR)) {}
    close(p[0]);
    int status = 0;
    uint64_t end = plat_now_ns() + (uint64_t)timeout_ms * 1000000u;
    while (waitpid(pid, &status, timeout_ms ? WNOHANG : 0) == 0) {
        if (plat_now_ns() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        plat_sleep_ms(10);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int plat_random(void *out, size_t n) {
    uint8_t *p = out;
    while (n) {
        ssize_t k = getrandom(p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

// ---- local IPC: a Unix stream socket. Reads with MSG_DONTWAIT, writes with MSG_NOSIGNAL (a closed peer is an error,
// not SIGPIPE).

struct PlatIpc { int fd; };

PlatIpc *plat_ipc_open(const char *name) {
    const char *dirs[4] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), "/tmp"};
    static const char *const subs[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
    for (size_t d = 0; d < 4; d++) {
        if (!dirs[d] || !*dirs[d]) continue;
        for (size_t s = 0; s < sizeof subs / sizeof *subs; s++) {
            struct sockaddr_un a = {.sun_family = AF_UNIX};
            int n = snprintf(a.sun_path, sizeof a.sun_path, "%s/%s%s", dirs[d], subs[s], name);
            if (n <= 0 || (size_t)n >= sizeof a.sun_path) continue;
            int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) return NULL;
            if (!connect(fd, (struct sockaddr *)&a, sizeof a)) {
                PlatIpc *c = malloc(sizeof *c);
                if (!c) return close(fd), NULL;
                c->fd = fd;
                return c;
            }
            close(fd);
        }
    }
    return NULL;
}

int plat_ipc_send(PlatIpc *c, const void *data, size_t n) {
    for (const char *p = data; n;) {
        ssize_t r = send(c->fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        p += r, n -= (size_t)r;
    }
    return 1;
}

long plat_ipc_recv(PlatIpc *c, void *buf, size_t cap) {
    for (;;) {
        ssize_t r = recv(c->fd, buf, cap, MSG_DONTWAIT);
        if (r > 0) return (long)r;
        if (r == 0) return -1;   // the peer closed
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}

void plat_ipc_close(PlatIpc *c) {
    if (!c) return;
    close(c->fd);
    free(c);
}

// ---- desktop

int plat_clipboard_set(const char *utf8) { return SDL_SetClipboardText(utf8); }

static void reap_main(void *arg) {
    waitpid((pid_t)(intptr_t)arg, NULL, 0);
}

// xdg-open picks the desktop's file manager; it runs detached, so a slow answer does not hold up the caller, and a
// thread of its own reaps it so it does not stay a zombie.
int plat_open_folder(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return 0;
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
    char *argv[] = {"xdg-open", (char *)path, NULL};
    pid_t pid;
    int e = posix_spawnp(&pid, "xdg-open", NULL, &a, argv, environ);
    posix_spawnattr_destroy(&a);
    if (e) return 0;
    plat_thread_spawn(reap_main, (void *)(intptr_t)pid, "reap", 64 << 10);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load: /proc/stat and /proc/self/stat for the CPU, hwmon for the temperature, the amdgpu sysfs files of
// the render device for the GPU.

struct PlatLoad {
    uint64_t busy, total, proc;   // the previous call's jiffies; total 0 before the first
    uint64_t proc_ns;
    char gpu[256];                // /sys/class/drm/cardN/device of the GPU in use; empty when unknown
    char temp[256];               // the CPU's hwmon temp1_input; empty when unknown
};

int read_u64_file(const char *path, uint64_t *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long x;
    int ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    if (ok) *v = x;
    return ok;
}

// The hwmon device of the CPU package: k10temp (AMD), coretemp (Intel), or the first that has a temp1_input.
static void find_cpu_temp(char *out, size_t cap) {
    out[0] = 0;
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[256], name[64] = "";
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        name[strcspn(name, "\n")] = 0;
        int cpu = !strcmp(name, "k10temp") || !strcmp(name, "coretemp") || !strcmp(name, "zenpower");
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
        if ((cpu || !out[0]) && plat_file_exists(path)) snprintf(out, cap, "%s", path);
        if (cpu && out[0]) break;
    }
    closedir(d);
}

// The DRM card with gpu_busy_percent (amdgpu); with several, the one with the most VRAM in use, as the game's device
// is not known by LUID on Linux.
static void find_gpu(char *out, size_t cap) {
    out[0] = 0;
    uint64_t best = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
        char dev[256], path[300];
        snprintf(dev, sizeof dev, "/sys/class/drm/%s/device", e->d_name);
        snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
        uint64_t v, used = 0;
        if (!read_u64_file(path, &v)) continue;
        snprintf(path, sizeof path, "%s/mem_info_vram_used", dev);
        read_u64_file(path, &used);
        if (!out[0] || used > best) snprintf(out, cap, "%s", dev), best = used;
    }
    closedir(d);
}

uint64_t plat_gpu_vram_mib(uint32_t vendor, uint32_t device, const uint8_t *luid) {
    (void)luid;
    uint64_t mib = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
        char path[300];
        unsigned v = 0, id = 0;
        uint64_t bytes;
        snprintf(path, sizeof path, "/sys/class/drm/%s/device/vendor", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int ok = fscanf(f, "%x", &v) == 1;
        fclose(f);
        snprintf(path, sizeof path, "/sys/class/drm/%s/device/device", e->d_name);
        if (!ok || v != vendor || !(f = fopen(path, "r"))) continue;
        ok = fscanf(f, "%x", &id) == 1;
        fclose(f);
        snprintf(path, sizeof path, "/sys/class/drm/%s/device/mem_info_vram_total", e->d_name);
        if (ok && id == device && read_u64_file(path, &bytes)) mib = bytes >> 20;
    }
    closedir(d);
    return mib;
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    (void)gpu_luid;
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    find_cpu_temp(l->temp, sizeof l->temp);
    find_gpu(l->gpu, sizeof l->gpu);
    return l;
}

void plat_load_close(PlatLoad *l) { free(l); }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[10] = {0};
    int n = f ? fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
                       &v[5], &v[6], &v[7], &v[8], &v[9])
              : 0;
    if (f) fclose(f);
    uint64_t pns = plat_process_cpu_ns();
    if (n >= 8) {
        uint64_t idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; i++) total += v[i];   // guest time is already inside user time
        uint64_t busy = total - idle;
        if (l->total && total > l->total) {
            uint64_t dt = total - l->total;
            *total_pct = 100.0f * (float)(busy - l->busy) / (float)dt;
            // the process time is in ns, the machine's in jiffies summed over every CPU
            double dt_ns = (double)dt * 1e9 / (double)sysconf(_SC_CLK_TCK);
            *proc_pct = (float)(100.0 * (double)(pns - l->proc_ns) / dt_ns);
        }
        l->busy = busy, l->total = total, l->proc_ns = pns;
    }
    uint64_t mc;
    if (l->temp[0] && read_u64_file(l->temp, &mc)) *temp_c = (float)mc / 1000.0f;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu[0]) return;
    char path[300];
    uint64_t v;
    snprintf(path, sizeof path, "%s/gpu_busy_percent", l->gpu);
    if (read_u64_file(path, &v)) *load_pct = (float)v;
    snprintf(path, sizeof path, "%s/mem_info_vram_used", l->gpu);
    if (read_u64_file(path, &v)) *vram_used_mb = (float)(v >> 20);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long total = 0, avail = 0, k;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &k) == 1) total = k;
            else if (sscanf(line, "MemAvailable: %llu kB", &k) == 1) avail = k;
        }
        fclose(f);
        if (total) *total_mb = (float)(total >> 10), *used_mb = (float)((total - avail) >> 10);
    }
    f = fopen("/proc/self/statm", "r");
    unsigned long long size, resident;
    if (f && fscanf(f, "%llu %llu", &size, &resident) == 2) *proc_mb = (float)(resident * (uint64_t)sysconf(_SC_PAGESIZE) >> 20);
    if (f) fclose(f);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    *resident = *private_bytes = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[128];
    unsigned long long k;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmRSS: %llu kB", &k) == 1) *resident = (uint64_t)k << 10;
        else if (sscanf(line, "RssAnon: %llu kB", &k) == 1) *private_bytes = (uint64_t)k << 10;
    }
    fclose(f);
}


// CPU time (user + system) of this process's threads with that name (/proc/self/task/*/comm), summed; -1 when none.
int64_t plat_threads_cpu_ns_named(const char *name) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    int64_t total = -1;
    long hz = sysconf(_SC_CLK_TCK);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[64], comm[64] = "", stat[1024];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
        fclose(f);
        comm[strcspn(comm, "\n")] = 0;
        if (strcmp(comm, name)) continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        size_t n = fread(stat, 1, sizeof stat - 1, f);
        fclose(f);
        stat[n] = 0;
        const char *p = strrchr(stat, ')');   // the fields after the name: state is 3, utime 14, stime 15
        unsigned long long ut = 0, st = 0;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
            total = (total < 0 ? 0 : total) + (int64_t)((ut + st) * (1000000000ull / (unsigned long long)hz));
    }
    closedir(d);
    return total;
}

// ---- desktop shortcut: the desktop folder from XDG_DESKTOP_DIR in ~/.config/user-dirs.dirs ("$HOME/Escritorio" on a
// Spanish desktop), else ~/Desktop.
static void desktop_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    char path[PATH_MAX], line[PATH_MAX];
    snprintf(out, cap, "%s/Desktop", home ? home : ".");
    const char *cfg = getenv("XDG_CONFIG_HOME");
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/user-dirs.dirs", cfg);
    else snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", home ? home : ".");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "XDG_DESKTOP_DIR=\"", 17)) continue;
        char *v = line + 17, *end = strchr(v, '"');
        if (!end) break;
        *end = 0;
        if (!strncmp(v, "$HOME", 5)) snprintf(out, cap, "%s%s", home ? home : "", v + 5);
        else snprintf(out, cap, "%s", v);
        break;
    }
    fclose(f);
}

static int write_desktop_entry(const char *path, const char *name, const char *exe, const char *dir) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    // Exec quotes the path (a folder with spaces); a quote or backslash in it would need escaping and is refused
    int ok = !strpbrk(exe, "\"\\`$") &&
             fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=\"%s\"\nPath=%s\nIcon=applications-games\n"
                        "Terminal=false\nCategories=Game;\n", name, exe, dir) > 0;
    ok = !fclose(f) && ok;
    if (!ok) unlink(path);
    return ok && !chmod(path, 0755);
}

int plat_desktop_shortcut(const char *name, const char *exe, const char *dir, char *out, size_t cap) {
    char desk[PATH_MAX], path[PATH_MAX + 300], menu[PATH_MAX + 300];
    desktop_dir(desk, sizeof desk);
    plat_file_mkdir_p(desk);
    snprintf(path, sizeof path, "%s/%s.desktop", desk, name);
    if (!write_desktop_entry(path, name, exe, dir)) return 0;
    // GNOME starts a desktop launcher only once it is trusted; KDE asks unless it is executable (done above)
    pid_t pid;
    char *argv[] = {"gio", "set", path, "metadata::trusted", "true", NULL};
    if (!posix_spawnp(&pid, "gio", NULL, NULL, argv, environ)) waitpid(pid, NULL, 0);
    const char *home = getenv("HOME");
    if (home) {   // the menu entry too, where application launchers look
        snprintf(menu, sizeof menu, "%s/.local/share/applications", home);
        plat_file_mkdir_p(menu);
        snprintf(menu + strlen(menu), sizeof menu - strlen(menu), "/%s.desktop", name);
        write_desktop_entry(menu, name, exe, dir);
    }
    if (out && cap) snprintf(out, cap, "%s", path);
    return 1;
}

// ---- a URL scheme (GameBanana's 1-Click: "scheme:https://gamebanana.com/mmdl/...") opening exe with arg and the URL:
// ~/.local/share/applications/<scheme>-url.desktop with x-scheme-handler/<scheme>, made the default for it in
// ~/.config/mimeapps.list through xdg-mime; rewritten only when it differs (the AppImage moved).
int plat_url_scheme(const char *scheme, const char *label, const char *exe, const char *arg) {
    const char *home = getenv("HOME");
    if (!home || strpbrk(exe, "\"\\`$")) return 0;
    char dir[PATH_MAX], path[PATH_MAX + 64], want[PATH_MAX * 2];
    snprintf(dir, sizeof dir, "%s/.local/share/applications", home);
    plat_file_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/%s-url.desktop", dir, scheme);
    snprintf(want, sizeof want,
             "[Desktop Entry]\nType=Application\nName=%s\nExec=\"%s\" %s %%u\nNoDisplay=true\nTerminal=false\n"
             "MimeType=x-scheme-handler/%s;\n", label, exe, arg, scheme);
    uint64_t n = 0;
    char *have = (char *)plat_file_read(path, &n);
    int same = have && n == strlen(want) && !memcmp(have, want, n);
    free(have);
    if (same) return 1;
    if (!plat_file_write_atomic(path, want, strlen(want))) return 0;
    char mime[96], file[96];
    snprintf(mime, sizeof mime, "x-scheme-handler/%s", scheme);
    snprintf(file, sizeof file, "%s-url.desktop", scheme);
    pid_t pid;
    char *argv[] = {"xdg-mime", "default", file, mime, NULL};
    if (!posix_spawnp(&pid, "xdg-mime", NULL, NULL, argv, environ)) waitpid(pid, NULL, 0);
    return 1;
}
