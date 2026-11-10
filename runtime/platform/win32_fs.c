// Windows implementation of platform.h, files and the file system (the parts: win32_int.h).
#include <io.h>
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
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

// ---- files

int widen(const char *s, wchar_t *w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (!n) w[0] = 0;
    return n;
}


int plat_file_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_dir_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    wchar_t w[WPATH], full[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD n = GetFullPathNameW(w, WPATH, full, NULL);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)cap, NULL, NULL) != 0;
}

void plat_file_mkdir_p(const char *path) {
    wchar_t tmp[WPATH];
    if (!widen(path, tmp, WPATH)) return;
    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *buf = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        buf = malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart)) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[sz.QuadPart] = 0;
            *size = (uint64_t)sz.QuadPart;
        }
    }
    CloseHandle(h);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

PlatAppend *plat_append_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatAppend *)h;
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    // Offset and OffsetHigh all ones: WriteFile appends at the end of the file, atomically against other writers.
    OVERLAPPED o = {.Offset = 0xffffffff, .OffsetHigh = 0xffffffff};
    DWORD put = 0;
    return WriteFile((HANDLE)f, data, (DWORD)n, &put, &o) && put == n;
}

void plat_append_sync(PlatAppend *f) {
    if (f) FlushFileBuffers((HANDLE)f);
}

void *plat_mmap_rw_file(const char *path, size_t size) {
    if (!path || !size) return NULL;
    char dir[WPATH];
    if (snprintf(dir, sizeof dir, "%s", path) < (int)sizeof dir) {
        char *s1 = strrchr(dir, '/'), *s2 = strrchr(dir, '\\');
        char *slash = s1 && (!s2 || s1 > s2) ? s1 : s2;
        if (slash && slash != dir) {
            *slash = 0;
            plat_file_mkdir_p(dir);
        }
    }
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READWRITE, (DWORD)((uint64_t)size >> 32), (DWORD)size, NULL);
    CloseHandle(h);
    if (!map) return NULL;
    void *v = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(map);
    return v;
}

int plat_file_replace(const char *from, const char *to) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return 0;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[WPATH + 48];
    wchar_t a[WPATH], b[WPATH];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, (unsigned)GetCurrentProcessId(), (unsigned)GetCurrentThreadId());
    if (len < 0 || len >= (int)sizeof tmp || !widen(tmp, a, WPATH) || !widen(path, b, WPATH)) return 0;
    HANDLE h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(a);
    return 0;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !_commit(_fileno(f)); }

void plat_file_delete(const char *path) {
    wchar_t w[WPATH];
    if (widen(path, w, WPATH)) DeleteFileW(w);
}

int64_t plat_file_size(const char *path) {
    wchar_t w[WPATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!widen(path, w, WPATH) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return -1;
    return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}

int plat_file_set_readonly(const char *path, int on) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return SetFileAttributesW(w, on ? a | FILE_ATTRIBUTE_READONLY : a & ~(DWORD)FILE_ATTRIBUTE_READONLY) != 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    wchar_t wd[WPATH], we[64], pat[WPATH + 64];
    if (!widen(dir, wd, WPATH) || !widen(ext, we, 64)) return 0;
    swprintf(pat, WPATH + 64, L"%ls\\*%ls", wd, we);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        if (!cb(name, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

