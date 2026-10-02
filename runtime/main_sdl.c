/* main_sdl.c - SDL2 front end: window, keyboard/controller, vsynced presentation.
 *   ./toxiccity --res res --save save [--scale 3] [--fps 60]
 * Keyboard: arrows, Enter/Space/Z = fire, A/F1 and S/F2 = soft keys,
 *           0-9 = number keys, [ = *, ] = #, Esc = quit.
 * Controller: D-pad left/right = 4/6; A/B/X = 2/8/5; LB/RB = 7/9;
 *             LT/RT = *, Y = #, Start = 0.
 * Sony Ericsson K800i key codes: up -1, down -2, left -3, right -4, fire -5, soft keys -6 / -7. */
#include "rt.h"
#include <SDL2/SDL.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

extern JClass *const jgame_midlet_class;
extern JClass J_k_class;
extern int32_t J_k_s11;
JObj *jgame_new_midlet(void);

static void audio_cb(void *ud, Uint8 *stream, int len) { (void)ud; rt_audio_mix((int16_t *)stream, len / 4); }

static int map_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return -1;    case SDLK_DOWN: return -2;
    case SDLK_LEFT: return -3;  case SDLK_RIGHT: return -4;
    case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: return -5;
    case SDLK_a: case SDLK_F1: return -6;
    case SDLK_s: case SDLK_F2: return -7;
    case SDLK_LEFTBRACKET: return 42;    /* '*' */
    case SDLK_RIGHTBRACKET: return 35;   /* '#' */
    default:
        if (k >= SDLK_0 && k <= SDLK_9) return 48 + (k - SDLK_0);
        return 0;
    }
}

/* Xbox layout -> game keypad (ASCII): D-pad L/R=4/6, A/B/X=2/8/5,
 * LB/RB=7/9, LT/RT=*, Y=#, Start=0. */
typedef struct { SDL_GameControllerButton button; int key; } PadMap;
static const PadMap pad_map[] = {
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, '4' },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, '6' },
    { SDL_CONTROLLER_BUTTON_A, '2' },
    { SDL_CONTROLLER_BUTTON_B, '8' },
    { SDL_CONTROLLER_BUTTON_X, '5' },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, '7' },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, '9' },
    { SDL_CONTROLLER_BUTTON_Y, '#' },
    { SDL_CONTROLLER_BUTTON_START, '0' },
};
#define PAD_MAP_N ((int)(sizeof pad_map / sizeof pad_map[0]))
enum { SRC_BUTTON = 1, SRC_LT = 2, SRC_RT = 4 };

static SDL_GameController *controller;
static SDL_JoystickID controller_id = -1;
static const int controller_keys[10] = { '4', '6', '2', '8', '5', '7', '9', '#', '0', '*' };
static uint8_t controller_sources[10];
static char controller_name[80] = "none detected";
static int controller_last_key, controller_last_down;
static int lt_down, rt_down;
static const char *controller_key_name(int key) {
    static char label[2]; label[0] = (char)key; label[1] = 0; return label;
}
static void controller_set_name(SDL_GameController *pad) {
    const char *name = pad ? SDL_GameControllerName(pad) : NULL;
    snprintf(controller_name, sizeof controller_name, "%s", name ? name : "none detected");
}
static void controller_release_all(void) {
    for (int i = 0; i < 10; i++) if (controller_sources[i]) rt_key_event(0, controller_keys[i]);
    memset(controller_sources, 0, sizeof controller_sources);
    lt_down = rt_down = 0;
}
static void controller_close(void) {
    controller_release_all();
    if (controller) SDL_GameControllerClose(controller);
    controller = NULL; controller_id = -1;
    controller_set_name(NULL);
}
static void controller_open_first(void) {
    if (controller) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        controller = SDL_GameControllerOpen(i);
        if (controller) {
            controller_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
            controller_set_name(controller);
            return;
        }
    }
}
/* Merge physical inputs that share a virtual key: LT and RT both map to '*'. */
static void controller_source(int key, uint8_t source, int pressed) {
    int i;
    for (i = 0; i < 10 && controller_keys[i] != key; i++) {}
    if (i == 10) return;
    uint8_t before = controller_sources[i];
    if (pressed) controller_sources[i] |= source;
    else controller_sources[i] &= (uint8_t)~source;
    if (!before && controller_sources[i]) {
        controller_last_key = key; controller_last_down = 1; rt_key_event(1, key);
    } else if (before && !controller_sources[i]) {
        controller_last_key = key; controller_last_down = 0; rt_key_event(0, key);
    }
}
static void controller_button(SDL_GameControllerButton b, int pressed) {
    for (int i = 0; i < PAD_MAP_N; i++)
        if (pad_map[i].button == b) { controller_source(pad_map[i].key, SRC_BUTTON, pressed); return; }
}
static void controller_axis(SDL_GameControllerAxis axis, int16_t value) {
    const int press = 16000, release = 8000; /* hysteresis avoids flicker at the trigger threshold */
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
        if (!lt_down && value > press) { lt_down = 1; controller_source('*', SRC_LT, 1); }
        else if (lt_down && value < release) { lt_down = 0; controller_source('*', SRC_LT, 0); }
    } else if (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        if (!rt_down && value > press) { rt_down = 1; controller_source('*', SRC_RT, 1); }
        else if (rt_down && value < release) { rt_down = 0; controller_source('*', SRC_RT, 0); }
    }
}

int main(int argc, char **argv) {
    int scale = 2, fps = 60, wide = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--res") && i + 1 < argc) rt_res_dir = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) rt_save_dir = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wide")) wide = 1;
    }
    if (scale < 1) scale = 1;
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
#ifdef _WIN32
    timeBeginPeriod(1); /* keep the Java repaint thread's short sleeps near 1 ms precision */
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GameControllerEventState(SDL_ENABLE);
    controller_open_first();
    int win_w = wide ? 1280 : RT_SCREEN_W * scale;
    int win_h = wide ? 720 : RT_SCREEN_H * scale;
    Uint32 win_flags = SDL_WINDOW_SHOWN | (wide ? SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED : 0);
    SDL_Window *win = SDL_CreateWindow("Spider-Man: Toxic City (recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, win_flags);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren) { fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError()); return 1; }
    if (!wide) SDL_RenderSetLogicalSize(ren, RT_SCREEN_W, RT_SCREEN_H);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");        /* crisp pixels */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, RT_SCREEN_W, RT_SCREEN_H);

    SDL_AudioSpec want; SDL_zero(want);
    want.freq = RT_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);
    else { fprintf(stderr, "[audio] no device (%s); using silent virtual clock\n", SDL_GetError()); rt_audio_virtual_start(NULL); }

    JvmTry base; base.prev = NULL; jvm_try_top = &base;
    if (setjmp(base.jb)) { fprintf(stderr, "[main] uncaught exception during startup\n"); return 2; }
    jvm_class_init(&J_k_class);
    J_k_s11 = 1000 / fps;
    JObj *m = jgame_new_midlet();
    void (*startApp)(JObj *) = jvm_find_method(jgame_midlet_class, "startApp", "()V");
    startApp(m);

    static uint32_t px[RT_SCREEN_W * RT_SCREEN_H]; uint64_t last = (uint64_t)-1; int running = 1;
    Uint64 perf_freq = SDL_GetPerformanceFrequency(), perf_start = SDL_GetPerformanceCounter();
    Uint64 frame_period = perf_freq / (Uint64)fps, next_frame = perf_start;
    uint64_t presented = 0, last_painted = rt_frame_counter();
    while (running && !rt_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                controller_open_first();
            }
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED && e.cdevice.which == controller_id) {
                controller_close();
                controller_open_first();
            }
            else if ((e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) &&
                     e.cbutton.which == controller_id)
                controller_button((SDL_GameControllerButton)e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
            else if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.which == controller_id)
                controller_axis((SDL_GameControllerAxis)e.caxis.axis, e.caxis.value);
            else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                int k = map_key(e.key.keysym.sym); if (k) rt_key_event(e.type == SDL_KEYDOWN, k);
            }
        }
        rt_canvas_service();
        uint64_t f = rt_frame_counter();
        if (f != last && rt_screen_snapshot(px)) { SDL_UpdateTexture(tex, NULL, px, RT_SCREEN_W * 4); last = f; }
        SDL_SetRenderDrawColor(ren, 12, 14, 22, 255); SDL_RenderClear(ren);
        if (wide) {
            int out_w, out_h; SDL_GetRendererOutputSize(ren, &out_w, &out_h);
            int dst_w = out_h * RT_SCREEN_W / RT_SCREEN_H;
            int dst_h = out_h;
            if (dst_w > out_w) { dst_w = out_w; dst_h = out_w * RT_SCREEN_H / RT_SCREEN_W; }
            SDL_Rect dst = { (out_w - dst_w) / 2, (out_h - dst_h) / 2, dst_w, dst_h };
            SDL_RenderCopy(ren, tex, NULL, &dst);
        } else SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        presented++;
        Uint64 now = SDL_GetPerformanceCounter();
        if (now - perf_start >= perf_freq) {
            double seconds = (double)(now - perf_start) / (double)perf_freq;
            uint64_t painted = rt_frame_counter();
            double present_fps = presented / seconds;
            double paint_fps = (painted - last_painted) / seconds;
            if (paint_fps > 0.0) {
                double desired_ms = 1000.0 / fps;
                double measured_ms = 1000.0 / paint_fps;
                double compensation = rt_sleep_get_compensation() + (measured_ms - desired_ms) * 0.6;
                if (compensation < 0.0) compensation = 0.0;
                if (compensation > desired_ms - 1.0) compensation = desired_ms - 1.0;
                rt_sleep_set_compensation((int32_t)(compensation + 0.5));
            }
            char title[320];
            if (controller_last_key)
                snprintf(title, sizeof title, "Spider-Man: Toxic City | Pad %s | Last key %s %s | Paint %.1f FPS | Display %.1f FPS | %.2f ms/frame | Target %d | Sleep trim %dms",
                         controller_name, controller_key_name(controller_last_key), controller_last_down ? "down" : "up",
                         paint_fps, present_fps, 1000.0 / present_fps, fps, rt_sleep_get_compensation());
            else
                snprintf(title, sizeof title, "Spider-Man: Toxic City | Pad %s | No pad input yet | Paint %.1f FPS | Display %.1f FPS | %.2f ms/frame | Target %d | Sleep trim %dms",
                         controller_name, paint_fps, present_fps, 1000.0 / present_fps, fps, rt_sleep_get_compensation());
            SDL_SetWindowTitle(win, title);
            perf_start = now; presented = 0; last_painted = painted;
        }
        next_frame += frame_period;
        now = SDL_GetPerformanceCounter();
        if (now < next_frame) {
            Uint64 left = next_frame - now;
            Uint32 ms = (Uint32)(left * 1000 / perf_freq);
            if (ms) SDL_Delay(ms);
        } else if (now - next_frame > frame_period) next_frame = now;
    }
    controller_close();
    SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    _exit(0);
}
