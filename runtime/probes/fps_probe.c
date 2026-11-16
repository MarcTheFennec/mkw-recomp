// --probe-fps: MKW_FPS=auto's controller (core/boot/fps_auto.c) on synthetic frame times. Each case feeds frames of
// a fixed length and checks where the interval settles and how often it changed: a steady frame must give one
// interval and no back and forth (the game's own rule taken to 4 would cycle 2, 3, 4 on 36 ms frames). Needs no game.
#include <stdio.h>

#include "host.h"

static int fails;

// The real period of a frame of ms at interval iv: the pacer holds it to the interval's period.
static double real_of(double ms, int iv) { return ms > iv * 1000.0 / 60 ? ms : iv * 1000.0 / 60; }

// frames of ms each (plus jitter of +-j alternating); returns the interval after them, counting the changes. real:
// the real frame time when the game's measure (ms) falls short of it, else 0 (the measure is the frame time)
static int feed_real(FpsAuto *a, double ms, double j, double real, int frames, int *changes) {
    for (int i = 0; i < frames; i++) {
        int prev = a->iv;
        double m = ms + (i & 1 ? j : -j);
        if (fps_auto_step(a, m, real_of(real > m ? real : m, prev)) != prev) (*changes)++;
    }
    return a->iv;
}

static int feed(FpsAuto *a, double ms, double j, int frames, int *changes) {
    return feed_real(a, ms, j, 0, frames, changes);
}

static void expect(const char *what, int iv, int want, int changes, int max_changes) {
    int ok = iv == want && changes <= max_changes;
    if (!ok) fails++;
    printf("%s fps: %-38s interval %d (want %d), %d changes (at most %d)\n", ok ? "ok  " : "FAIL", what, iv, want,
           changes, max_changes);
}

int probe_fps(void) {
    static const struct { const char *what; double ms, jitter; int want; } steady[] = {
        {"15 ms frames (fits 60)", 15.0, 1.0, 1},       {"16 ms frames (just fits 60)", 16.0, 0.3, 1},
        {"20 ms frames", 20.0, 2.0, 2},                 {"36 ms frames (Radeon 660M race)", 36.0, 3.0, 3},
        {"45 ms frames", 45.0, 4.0, 3},                 {"55 ms frames", 55.0, 4.0, 4},
        {"120 ms frames (slower than 15)", 120.0, 10.0, 4},
    };
    for (unsigned i = 0; i < sizeof steady / sizeof steady[0]; i++) {
        FpsAuto a;
        fps_auto_reset(&a);
        int changes = 0;
        feed(&a, steady[i].ms, steady[i].jitter, 120, &changes);   // settle
        int settled = changes;
        changes = 0;
        int iv = feed(&a, steady[i].ms, steady[i].jitter, 1200, &changes);   // 20 s at 60 Hz: no more changes
        expect(steady[i].what, iv, steady[i].want, changes, 0);
        if (settled > 2) fails++, printf("FAIL fps: %s took %d changes to settle\n", steady[i].what, settled);
    }

    FpsAuto a;   // a loading hitch of 4 frames at 60 fps: the median of 8 does not move
    fps_auto_reset(&a);
    int changes = 0;
    feed(&a, 15.0, 1.0, 120, &changes);
    feed(&a, 200.0, 0.0, 3, &changes);
    int iv = feed(&a, 15.0, 1.0, 120, &changes);
    expect("15 ms with a 3-frame 200 ms hitch", iv, 1, changes, 0);

    fps_auto_reset(&a);   // a load (36 ms race, then 12 ms menu): back to 60 within about two seconds
    changes = 0;
    feed(&a, 36.0, 3.0, 300, &changes);
    iv = feed(&a, 12.0, 1.0, 150, &changes);   // 15 +- 1 would not: its median, 16, is in the hysteresis gap
    expect("36 ms then 12 ms, 150 frames later", iv, 1, changes, 3);

    // lat6: the game measures 9 ms while frames take 60: a step down is undone and the hold doubles, so in 60 s at
    // most a handful of trials
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 9.0, 0.5, 60.0, 60, &changes);
    int c0 = changes;
    changes = 0;
    iv = feed_real(&a, 9.0, 0.5, 60.0, 900, &changes);   // 60 s at 15 fps
    expect("game measures 9 ms, frames take 60", iv, 4, changes + (c0 > 3) * 99, 8);

    fps_auto_reset(&a);   // lat4: race frames spread over 25-40 ms, around 30 fps: one interval, no back and forth
    changes = 0;
    static const double spread[] = {25, 31, 38, 27, 40, 29, 33, 26, 36, 30, 28, 39, 32, 34, 27, 31};
    for (int i = 0; i < 300; i++) {
        int prev = a.iv;
        if (fps_auto_step(&a, spread[i % 16], real_of(spread[i % 16], prev)) != prev) changes++;
    }
    expect("frames spread over 25-40 ms", a.iv, a.iv == 2 ? 2 : 3, changes, 1);

    // lat5: the game measured 14 ms while its frames took 21 (the race at 0.79x at interval 1): up on the real period,
    // and back down at most a couple of times in 40 s (the hold doubles)
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 14.0, 0.5, 21.0, 30, &changes);
    int after = a.iv;
    feed_real(&a, 14.0, 0.5, 21.0, 2400, &changes);
    expect("game measures 14 ms, frames take 21", after, 2, changes, 5);

    // mp1/mp2 (Steam Deck, 2 players): 38 ms measured, 45 ms frames (interval 3) and, every ~20 s, stalls of ~300 ms
    // frames for ~8 s (the CPU held at 400 MHz). No interval holds a 300 ms frame: going to 4 for each one, then
    // holding it (doubled on each return) kept the race at 15 fps where it runs 20 between the stalls.
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 38.0, 2.0, 45.0, 120, &changes);
    int at3 = 0, normal = 0;
    for (int cycle = 0; cycle < 8; cycle++) {
        for (int i = 0; i < 27; i++) {
            int prev = a.iv;
            if (fps_auto_step(&a, 300.0, 300.0) != prev) changes++;
        }
        for (int i = 0; i < 240; i++) {
            int prev = a.iv;
            double m = 38.0 + (i & 1 ? 2.0 : -2.0);
            if (fps_auto_step(&a, m, real_of(45.0, prev)) != prev) changes++;
            normal++, at3 += a.iv == 3;
        }
    }
    int pct = normal ? at3 * 100 / normal : 0;
    printf("     fps: stalls of 300 ms between 45 ms frames: interval 3 in %d %% of the frames between them\n", pct);
    expect("45 ms frames with 8 s stalls of 300 ms", a.iv, 3, changes + (pct < 90) * 99, 4);

    // a machine that is that slow from the start: to 4 at once, no stall allowance (nothing fitted before)
    fps_auto_reset(&a);
    changes = 0;
    iv = feed(&a, 200.0, 5.0, 10, &changes);   // 2 s of 200 ms frames
    expect("200 ms frames from the start, 10 frames", iv, 4, changes, 1);

    // fitting 45 ms frames, then 200 ms frames for good: after the stall allowance it goes to 4
    fps_auto_reset(&a);
    changes = 0;
    feed_real(&a, 38.0, 2.0, 45.0, 120, &changes);
    changes = 0;
    iv = feed(&a, 200.0, 5.0, 300, &changes);   // 60 s
    expect("45 ms, then 200 ms frames for 60 s", iv, 4, changes, 1);

    printf("fps: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
