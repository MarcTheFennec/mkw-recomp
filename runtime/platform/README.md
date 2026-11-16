# runtime/platform

| File | What it is |
|---|---|
| `platform.h` | The operating system interface the rest of the runtime uses |
| `linux.c`, `linux_int.h` | Linux: threads, preemption through signals, fibers; the header is what the `linux_*.c` parts share |
| `linux_sync.c` | Linux: clock, locks and condition variables on futex |
| `linux_fs.c` | Linux: files and file system (atomic write, rename without replace) |
| `linux_mem.c` | Linux: virtual memory, write watching with userfaultfd (Linux 6.7+), fault handler; `plat_mem_poke` writes through the protection via `/proc/self/mem` |
| `linux_sys.c` | Linux: process, local IPC (Unix sockets), desktop, libraries, machine load (CPU, temperature, GPU) and desktop shortcut |
| `win32.c`, `win32_int.h` | Windows: threads, preemption (SuspendThread), fibers, clock, locks; the header is what the `win32_*.c` parts share |
| `win32_fs.c` | Windows: files and file system (UTF-16 paths) |
| `win32_mem.c` | Windows: virtual memory and write watching (`MEM_WRITE_WATCH`) |
| `win32_sys.c` | Windows: process, local IPC (named pipes), desktop and libraries |
| `win32_load.c` | Windows: machine load (CPU, PDH for temperature and GPU) and desktop shortcut |
| `common.c`, `fp_x86.h` | Common parts, x86 floating point control |
| `mkw.rc`, `mkw.manifest` | `mkw.exe`'s manifest (UTF-8 code page, from totk-pc-recomp); `build.py --target windows` |
