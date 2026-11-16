// Windows implementation of platform.h, process, desktop and dynamic libraries (the parts: win32_int.h).
#include <io.h>
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "platform.h"

#include "win32_int.h"

// ---- process

void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    ExitProcess((UINT)code);
}

// _putenv_s, not SetEnvironmentVariableA: the CRT's getenv (cfg.c) reads its own copy of the environment taken at start,
// which only the CRT's functions update (both copies); an empty value removes the variable.
int plat_setenv(const char *name, const char *value) { return _putenv_s(name, value ? value : "") == 0; }

uint32_t plat_process_id(void) { return GetCurrentProcessId(); }

int plat_process_alive(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}


uint64_t plat_process_cpu_ns(void) {
    FILETIME cr, ex, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return 0;
    return (ft64(k) + ft64(u)) * 100;
}

uint64_t plat_process_uptime_ns(void) {
    FILETIME cr, ex, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    return ft64(now) > ft64(cr) ? (ft64(now) - ft64(cr)) * 100 : 0;
}

uint64_t plat_unix_time_ns(void) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (ft64(now) - 116444736000000000ull) * 100;   // FILETIME counts 100 ns from 1601
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_s(out, &t);
}

// localtime_s and gmtime_s take 1970 to 3000 (PLAT_TIME_MAX); outside it they are an invalid parameter, which ends
// the process (#310), so the range is checked first, as linux_sys.c does.
int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && !localtime_s(out, &t);
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && !gmtime_s(out, &t);
}

long plat_tz_west(void) {
    long west = 0;
    _tzset();
    _get_timezone(&west);
    return west;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                       .hStdOutput = wr, .hStdError = wr};
    PROCESS_INFORMATION pi;
    wchar_t *cmdline = _wcsdup(GetCommandLineW());
    SetEnvironmentVariableA(env_name, env_value);
    BOOL ok = cmdline && CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA(env_name, NULL);
    free(cmdline);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    size_t len = 0;
    DWORD got;
    char sink[512];
    while (len + 1 < cap && ReadFile(rd, out + len, (DWORD)(cap - 1 - len), &got, NULL) && got) len += got;
    if (cap) out[len] = 0;
    while (ReadFile(rd, sink, sizeof sink, &got, NULL) && got) {}
    CloseHandle(rd);
    if (WaitForSingleObject(pi.hProcess, timeout_ms ? timeout_ms : INFINITE) != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1), WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int plat_random(void *out, size_t n) {
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// FILE_FLAG_OVERLAPPED handles: a read names its offset, and each thread waits on an event of its own, so concurrent
// reads on one handle do not share the file pointer or the event.
PlatFile *plat_file_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

void plat_file_close(PlatFile *f) {
    if (f) CloseHandle((HANDLE)f);
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    static __thread HANDLE ev;
    if (!ev && !(ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD got = 0;
    if (ReadFile((HANDLE)f, buf, n, &got, &ov)) return got;
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) {
        if (GetOverlappedResult((HANDLE)f, &ov, &got, TRUE)) return got;
        e = GetLastError();
    }
    return e == ERROR_HANDLE_EOF ? 0 : e == ERROR_NOACCESS ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle((HANDLE)f, &i)) return 0;
    *out = (PlatFileId){i.dwVolumeSerialNumber, (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow,
                        (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow,
                        (uint64_t)i.ftLastWriteTime.dwHighDateTime << 32 | i.ftLastWriteTime.dwLowDateTime,
                        (uint64_t)i.ftCreationTime.dwHighDateTime << 32 | i.ftCreationTime.dwLowDateTime};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    HANDLE map = CreateFileMappingW((HANDLE)f, NULL, PAGE_READONLY, 0, 0, NULL);
    const void *v = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (map) CloseHandle(map);   // the view keeps the mapping alive
    return v;
}

static __thread DWORD fs_last_error;
int plat_fs_os_error(void) { return (int)fs_last_error; }

static int fs_err(DWORD e) {
    fs_last_error = e;
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_DIRECTORY: return PLAT_FS_NOT_FOUND;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return PLAT_FS_EXISTS;
    case ERROR_DIR_NOT_EMPTY: return PLAT_FS_NOT_EMPTY;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

// A symbolic link or junction is a link: removed as a link, never followed. Any other reparse point (OneDrive's cloud
// placeholders, deduplicated files) is an ordinary file or directory whose content the file system fetches on access.
static int fs_kind_of(const WIN32_FIND_DATAW *fd) {
    DWORD a = fd->dwFileAttributes;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT && (fd->dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd->dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
        return PLAT_FS_LINK;
    return a & FILE_ATTRIBUTE_DIRECTORY ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_kind(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_MISSING;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);   // the reparse tag comes with the find data only
    if (h == INVALID_HANDLE_VALUE) return PLAT_FS_MISSING;
    FindClose(h);
    return fs_kind_of(&fd);
}

int plat_fs_mkdir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    return CreateDirectoryW(w, NULL) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_rmdir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    if (RemoveDirectoryW(w)) return PLAT_FS_OK;
    DWORD e = GetLastError(), a = GetFileAttributesW(w);
    if (e == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        // a read-only directory (copied over from another emulator's save folder) refuses RemoveDirectoryW
        SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
        if (RemoveDirectoryW(w)) return PLAT_FS_OK;
        e = GetLastError();
    }
    return fs_err(e);
}

int plat_fs_unlink(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(w) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_rename(const char *from, const char *to, int durable) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return PLAT_FS_ERROR;
    return MoveFileExW(a, b, durable ? MOVEFILE_WRITE_THROUGH : 0) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_link(const char *existing, const char *path) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(existing, a, WPATH) || !widen(path, b, WPATH)) return PLAT_FS_ERROR;
    return CreateHardLinkW(b, a, NULL) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_links(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION fi;
    int links = f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &fi) ? (int)fi.nNumberOfLinks : 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return links;
}

// NTFS journals directory entries; flushing the directory handle is as far as Windows goes.
void plat_fs_sync_dir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}

// Restart Manager (rstrtmgr.dll, loaded on first use) names the processes that hold a file open.
typedef DWORD(WINAPI *RmStartSession_t)(DWORD *, DWORD, WCHAR *);
typedef DWORD(WINAPI *RmRegisterResources_t)(DWORD, UINT, LPCWSTR *, UINT, RM_UNIQUE_PROCESS *, UINT, LPCWSTR *);
typedef DWORD(WINAPI *RmGetList_t)(DWORD, UINT *, UINT *, RM_PROCESS_INFO *, LPDWORD);
typedef DWORD(WINAPI *RmEndSession_t)(DWORD);
static struct {
    INIT_ONCE once;
    RmStartSession_t start;
    RmRegisterResources_t add;
    RmGetList_t list;
    RmEndSession_t end;
} rm = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK rm_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("rstrtmgr.dll");
    if (!l) return TRUE;
    rm.add = (RmRegisterResources_t)(void *)GetProcAddress(l, "RmRegisterResources");
    rm.list = (RmGetList_t)(void *)GetProcAddress(l, "RmGetList");
    rm.end = (RmEndSession_t)(void *)GetProcAddress(l, "RmEndSession");
    RmStartSession_t start = (RmStartSession_t)(void *)GetProcAddress(l, "RmStartSession");
    if (rm.add && rm.list && rm.end) rm.start = start;   // start set: all of them are there
    return TRUE;
}

// The files under dir (links not followed), at most BUSY_FILES of them, each a malloc'd path.
#define BUSY_FILES 256
typedef struct { LPCWSTR p[BUSY_FILES]; UINT n; } BusyFiles;

static void busy_files(const wchar_t *dir, BusyFiles *f) {
    wchar_t pat[WPATH + 4];
    swprintf(pat, WPATH + 4, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DWORD a = fd.dwFileAttributes;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        size_t n = wcslen(dir) + wcslen(fd.cFileName) + 2;
        wchar_t *p = n <= WPATH ? malloc(n * sizeof *p) : NULL;
        if (!p) continue;
        swprintf(p, n, L"%ls\\%ls", dir, fd.cFileName);
        if (a & FILE_ATTRIBUTE_DIRECTORY) busy_files(p, f), free(p);
        else f->p[f->n++] = p;
    } while (f->n < BUSY_FILES && FindNextFileW(h, &fd));
    FindClose(h);
}

void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    if (!cap) return;
    *out = 0;
    wchar_t w[WPATH];
    if (!widen(dir, w, WPATH)) return;
    for (wchar_t *q = w; *q; q++)
        if (*q == L'/') *q = L'\\';
    // a rename needs DELETE access to the directory: a folder made by an elevated run can refuse it
    HANDLE h = CreateFileW(w, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) snprintf(out, cap, "no permission to rename it");
        return;
    }
    CloseHandle(h);
    InitOnceExecuteOnce(&rm.once, rm_load, NULL, NULL);
    DWORD session;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {0};
    if (!rm.start || rm.start(&session, 0, key) != ERROR_SUCCESS) return;
    BusyFiles f = {.n = 0};
    busy_files(w, &f);
    RM_PROCESS_INFO info[8];
    UINT need = 0, n = 8;
    DWORD reasons = 0, e = f.n ? rm.add(session, f.n, f.p, 0, NULL, 0, NULL) : ERROR_SUCCESS;
    if (f.n && e == ERROR_SUCCESS) e = rm.list(session, &need, &n, info, &reasons);
    else n = 0;
    rm.end(session);
    for (UINT i = 0; i < f.n; i++) free((void *)f.p[i]);
    if (e == ERROR_MORE_DATA) snprintf(out, cap, "%u programs hold files in it", need);   // info is not filled then
    if (e != ERROR_SUCCESS) return;
    size_t at = 0;
    for (UINT i = 0; i < n && at < cap; i++) {   // the program's display name ("Windows Explorer") and process id
        char name[CCH_RM_MAX_APP_NAME + 1];
        if (!WideCharToMultiByte(CP_UTF8, 0, info[i].strAppName, -1, name, sizeof name, NULL, NULL)) strcpy(name, "?");
        int k = snprintf(out + at, cap - at, "%s%s %lu", i ? ", " : "", name, info[i].Process.dwProcessId);
        if (k < 0) break;
        at += (size_t)k;
    }
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    wchar_t wd[WPATH], pat[WPATH + 4];
    if (!widen(dir, wd, WPATH)) return PLAT_FS_ERROR;
    swprintf(pat, WPATH + 4, L"%ls\\*", wd);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return fs_err(GetLastError());
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        int kind = fs_kind_of(&fd);
        if (!cb(name, kind, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return PLAT_FS_OK;
}

static PlatFile *fs_open(const char *path, DWORD access, DWORD share, DWORD how, int *err) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) {
        if (err) *err = PLAT_FS_ERROR;
        return NULL;
    }
    HANDLE h = CreateFileW(w, access, share, NULL, how, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = fs_err(GetLastError());
        return NULL;
    }
    return (PlatFile *)h;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    return fs_open(path, GENERIC_READ | (write ? GENERIC_WRITE : 0),
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, err);
}

PlatFile *plat_fs_create(const char *path, int *err) { return fs_open(path, GENERIC_WRITE, 0, CREATE_NEW, err); }

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    static __thread HANDLE ev;
    if (!ev && !(ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD put = 0;
    if (WriteFile((HANDLE)f, buf, n, &put, &ov)) return put;
    if (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult((HANDLE)f, &ov, &put, TRUE)) return put;
    return -1;
}

int plat_file_set_size(PlatFile *f, uint64_t size) {
    FILE_END_OF_FILE_INFO eof = {.EndOfFile.QuadPart = (LONGLONG)size};
    return SetFileInformationByHandle((HANDLE)f, FileEndOfFileInfo, &eof, sizeof eof) ? PLAT_FS_OK
                                                                                      : fs_err(GetLastError());
}

int plat_file_fsync(PlatFile *f) { return FlushFileBuffers((HANDLE)f) != 0; }

int plat_temp_dir(char *out, size_t cap) {
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (!n || n > MAX_PATH) return 0;
    while (n && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

uintptr_t plat_exe_base(void) { return (uintptr_t)GetModuleHandleW(NULL); }

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    HMODULE m = NULL;
    wchar_t w[WPATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) ||
        !m || !GetModuleFileNameW(m, w, WPATH))
        return 0;
    const wchar_t *n = wcsrchr(w, L'\\') ? wcsrchr(w, L'\\') + 1 : w;
    *base = (uintptr_t)m;
    return WideCharToMultiByte(CP_UTF8, 0, n, -1, name, (int)cap, NULL, NULL) != 0;
}

int plat_exe_path(char *out, size_t cap) {
    wchar_t w[WPATH];
    DWORD n = GetModuleFileNameW(NULL, w, WPATH);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    wchar_t wn[256], v[WPATH];
    if (!widen(name, wn, 256)) return 0;
    DWORD n = GetEnvironmentVariableW(wn, v, WPATH);
    if (!n || n >= WPATH) return 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, v, -1, out, (int)cap, NULL, NULL);
    return m ? (size_t)m - 1 : 0;
}

uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }

// Windows names its zones its own way ("Romance Standard Time"); the ICU it ships since Windows 10 1903 (icu.dll) gives
// the IANA name of the default zone.
int plat_time_zone(char *out, size_t cap) {
    typedef int32_t (*DefTz)(uint16_t *, int32_t, int *);
    HMODULE icu = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    DefTz def = icu ? (DefTz)(void *)GetProcAddress(icu, "ucal_getDefaultTimeZone") : NULL;
    uint16_t w[64];
    int status = 0, ok = 0;
    int32_t n = def ? def(w, 64, &status) : 0;
    if (def && status <= 0 && n > 0 && n < 64 && (size_t)n < cap) {
        for (int32_t i = 0; i < n; i++) out[i] = w[i] < 0x80 ? (char)w[i] : '_';
        out[n] = 0, ok = 1;
    }
    if (icu) FreeLibrary(icu);
    return ok;
}

const char *plat_ui_language(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        LCID lcid = GetUserDefaultUILanguage();
        int n = LCIDToLocaleName(lcid, w, LOCALE_NAME_MAX_LENGTH, 0);
        if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, sizeof tag, NULL, NULL)) tag[0] = 0;
        done = 1;
    }
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        size_t i = 0;
        if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH))
            for (; w[i] && w[i] < 0x80 && i + 1 < sizeof tag; i++) tag[i] = (char)w[i];
        tag[i] = 0;
        done = 1;
    }
    return tag;
}

int plat_font_path(const char *lang, char *out, size_t cap) {
    static const struct { const char *lang; const wchar_t *files[3]; } fonts[] = {
        {"ja", {L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc"}},
        {"ko", {L"malgun.ttf", L"gulim.ttc", NULL}},
        {"zh-Hans", {L"msyh.ttc", L"simsun.ttc", NULL}},
        {"zh-Hant", {L"msjh.ttc", L"mingliu.ttc", NULL}},
        {"th", {L"LeelawUI.ttf", L"tahoma.ttf", NULL}},
    };
    wchar_t dir[MAX_PATH], w[WPATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; i++) {
        if (strcmp(fonts[i].lang, lang)) continue;
        for (int k = 0; k < 3 && fonts[i].files[k]; k++) {
            swprintf(w, WPATH, L"%ls\\Fonts\\%ls", dir, fonts[i].files[k]);
            DWORD a = GetFileAttributesW(w);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
        }
    }
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    // GetVersionEx reports the version the manifest asks for; ntdll's RtlGetVersion the real one
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t get = nt ? (RtlGetVersion_t)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (!get || get(&v) != 0) return 0;
    DWORD ubr = 0, size = sizeof ubr;   // the update build revision, the number after the build
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                     NULL, &ubr, &size) != ERROR_SUCCESS)
        ubr = 0;
    snprintf(out, cap, ubr ? "Windows %lu.%lu.%lu.%lu" : "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
             v.dwBuildNumber, ubr);
    return 1;
}

int plat_stderr_path(char *out, size_t cap) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t w[WPATH];
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return 0;
    DWORD n = GetFinalPathNameByHandleW(h, w, WPATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= WPATH) return 0;
    const wchar_t *p = wcsncmp(w, L"\\\\?\\", 4) ? w : w + 4;   // the \\?\ prefix
    return WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL) != 0;
}

// ---- local IPC: a named pipe opened for synchronous reads and writes, read only as far as PeekNamedPipe says is
// there, so a read never blocks (a blocking ReadFile on a synchronous handle would also hold back its writes).

struct PlatIpc { HANDLE h; };

PlatIpc *plat_ipc_open(const char *name) {
    wchar_t w[256] = L"\\\\.\\pipe\\";
    size_t n = wcslen(w);
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, w + n, (int)(256 - n))) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;   // no such pipe, or every instance busy (ERROR_PIPE_BUSY)
    PlatIpc *c = malloc(sizeof *c);
    if (!c) return CloseHandle(h), NULL;
    c->h = h;
    return c;
}

int plat_ipc_send(PlatIpc *c, const void *data, size_t n) {
    for (const char *p = data; n;) {
        DWORD done = 0, part = n > 0x10000 ? 0x10000 : (DWORD)n;
        if (!WriteFile(c->h, p, part, &done, NULL) || !done) return 0;
        p += done, n -= done;
    }
    return 1;
}

long plat_ipc_recv(PlatIpc *c, void *buf, size_t cap) {
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(c->h, NULL, 0, NULL, &avail, NULL)) return -1;
    if (!avail || !cap) return 0;
    if (!ReadFile(c->h, buf, avail < cap ? avail : (DWORD)cap, &got, NULL)) return -1;
    return (long)got;
}

void plat_ipc_close(PlatIpc *c) {
    if (!c) return;
    CloseHandle(c->h);
    free(c);
}

// ---- desktop

int plat_clipboard_set(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem) return 0;
    wchar_t *w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    GlobalUnlock(mem);
    // A clipboard opened without a window gets no owner, and SetClipboardData may then fail: a message-only window of
    // the calling thread owns it for the call.
    HWND owner = CreateWindowExW(0, L"STATIC", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    int ok = 0;
    if (OpenClipboard(owner)) {
        ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem) != NULL;
        CloseClipboard();
    }
    if (owner) DestroyWindow(owner);
    if (!ok) GlobalFree(mem);   // after a successful SetClipboardData the system owns it
    return ok;
}

static DWORD WINAPI open_folder_main(void *arg) {
    ShellExecuteW(NULL, L"open", arg, NULL, NULL, SW_SHOWNORMAL);
    free(arg);
    return 0;
}

int plat_open_folder(const char *path) {
    wchar_t *w = malloc(WPATH * sizeof *w);
    if (!w || !widen(path, w, WPATH)) return free(w), 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return free(w), 0;
    // the shell can take a while to answer: not on the caller's thread (the present path)
    HANDLE t = CreateThread(NULL, 0, open_folder_main, w, 0, NULL);
    if (!t) return free(w), 0;
    CloseHandle(t);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) {
    wchar_t w[WPATH];
    if (!widen(name, w, WPATH)) return NULL;
    int bare = !wcschr(w, L'\\') && !wcschr(w, L'/');
    return (void *)LoadLibraryExW(w, NULL, bare ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0);
}

void *plat_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }

