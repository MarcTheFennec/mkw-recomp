// Exercise the production integer-image patch without a GPU or game files (test_spirv_images.py).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"

void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    rewind(f);
    if (bytes < 0 || bytes % 4) return 2;
    uint32_t *w = malloc((size_t)bytes + 4), *before = malloc((size_t)bytes + 4);
    if (!w || !before || fread(w, 1, (size_t)bytes, f) != (size_t)bytes) return 2;
    fclose(f);
    memcpy(before, w, (size_t)bytes);
    uint64_t n = 123;
    const char *why = "not cleared";
    uint32_t *out = spirv_int_images(w, (uint64_t)bytes / 4, strtoull(argv[3], NULL, 0),
                                     strtoull(argv[4], NULL, 0), &n, &why);
    if (memcmp(before, w, (size_t)bytes)) return 3;
    free(before);
    free(w);
    if (!out) {
        if (n || !why) return 3;
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    if (why) return 3;
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 4, (size_t)n, f) != n) return 2;
    fclose(f);
    free(out);
    return 0;
}
