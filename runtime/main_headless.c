/* main_headless.c - runs the recompiled game without a window.
 *   ./toxiccity_headless [--wav out.wav] --res res --save save --seconds 10 --shot-dir shots --shot-ms 1000
 *                        --script "2000:-5;3000:-1"        (ms:keycode, pressed for 120 ms)
 * Writes PNG screenshots of the painted frames, so rendering can be checked on any machine. */
#include "rt.h"
#include <unistd.h>
#include <time.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

extern JClass *const jgame_midlet_class;
JObj *jgame_new_midlet(void);

static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static int shot(const char *dir, int n) {
    static uint32_t px[RT_SCREEN_W * RT_SCREEN_H]; static uint8_t rgb[RT_SCREEN_W * RT_SCREEN_H * 3];
    if (!rt_screen_snapshot(px)) return 0;
    for (int i = 0; i < RT_SCREEN_W * RT_SCREEN_H; i++) { rgb[i * 3] = px[i] >> 16; rgb[i * 3 + 1] = px[i] >> 8; rgb[i * 3 + 2] = px[i]; }
    char p[512]; snprintf(p, sizeof p, "%s/shot_%02d.png", dir, n);
    return stbi_write_png(p, RT_SCREEN_W, RT_SCREEN_H, 3, rgb, RT_SCREEN_W * 3);
}

typedef struct { int64_t at; int key; int stage; } KeyEv;

int main(int argc, char **argv) {
    int seconds = 3, shot_ms = 1000; const char *shot_dir = NULL, *script = NULL, *wav = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--res") && i + 1 < argc) rt_res_dir = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) rt_save_dir = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot-dir") && i + 1 < argc) shot_dir = argv[++i];
        else if (!strcmp(argv[i], "--shot-ms") && i + 1 < argc) shot_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
    }
    KeyEv evs[256]; int nev = 0;
    if (script) for (char *c = (char *)script; *c && nev < 255;) {
        char *e; long at = strtol(c, &e, 10); if (*e != ':') break; long key = strtol(e + 1, &e, 10);
        evs[nev++] = (KeyEv){ at, (int)key, 0 }; evs[nev++] = (KeyEv){ at + 120, (int)key, 1 };
        c = *e == ';' ? e + 1 : e; if (!*e) break;
    }
    JvmTry base; base.prev = NULL; jvm_try_top = &base;
    if (setjmp(base.jb)) {
        JThrowable *ex = (JThrowable *)jvm_exc; fprintf(stderr, "[main] uncaught %s\n", ex ? ex->o.cls->name : "?");
        rt_dump_counts(stderr); return 2;
    }
    rt_audio_virtual_start(wav);
    JObj *m = jgame_new_midlet();
    void (*startApp)(JObj *) = jvm_find_method(jgame_midlet_class, "startApp", "()V");
    if (!startApp) { fprintf(stderr, "no startApp\n"); return 1; }
    startApp(m);
    fprintf(stderr, "[harness] started; running %d s\n", seconds);

    int64_t t0 = now_ms(), next_shot = t0 + shot_ms; int nshot = 0, ei = 0;
    for (;;) {
        rt_canvas_service();
        int64_t t = now_ms() - t0;
        while (ei < nev && evs[ei].at <= t) { rt_key_event(evs[ei].stage == 0, evs[ei].key); ei++; }
        if (shot_dir && now_ms() >= next_shot) { if (shot(shot_dir, nshot)) nshot++; next_shot += shot_ms; }
        usleep(5000);
        if (rt_quit || t >= seconds * 1000) break;
    }
    fprintf(stderr, "[harness] done (quit=%d, frames=%llu). API call counts:\n", (int)rt_quit, (unsigned long long)rt_frame_counter());
    rt_dump_counts(stderr); fflush(stderr);
    rt_audio_virtual_stop();
    _exit(0);
}
