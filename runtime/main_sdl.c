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

/* Map controller buttons to the game's numeric/star/hash key codes. */
static SDL_GameController *controller;
static uint8_t controller_sources[10];
static const int controller_keys[10] = { 52, 54, 50, 56, 53, 55, 57, 35, 48, 42 };
static void controller_source(int key, uint8_t source, int pressed) {
    int i;
    for (i = 0; i < 10 && controller_keys[i] != key; i++) {}
    if (i == 10) return;
    uint8_t before = controller_sources[i];
    if (pressed) controller_sources[i] |= source;
    else controller_sources[i] &= (uint8_t)~source;
    if (!before && controller_sources[i]) rt_key_event(1, key);
    else if (before && !controller_sources[i]) rt_key_event(0, key);
}
static void controller_button(SDL_GameControllerButton b, int pressed) {
    int key = 0;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key = 52; break; /* 4 */
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key = 54; break; /* 6 */
    case SDL_CONTROLLER_BUTTON_A: key = 50; break;          /* 2 */
    case SDL_CONTROLLER_BUTTON_B: key = 56; break;          /* 8 */
    case SDL_CONTROLLER_BUTTON_X: key = 53; break;          /* 5 */
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: key = 55; break;  /* 7 */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: key = 57; break; /* 9 */
    case SDL_CONTROLLER_BUTTON_Y: key = 35; break;          /* # */
    case SDL_CONTROLLER_BUTTON_START: key = 48; break;      /* 0 */
    default: return;
    }
    controller_source(key, 1, pressed);
}
static void controller_axis(SDL_GameControllerAxis axis, int16_t value) {
    const int threshold = 12000;
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
        controller_source(42, 1, value > threshold);  /* LT = * */
    else if (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        controller_source(42, 2, value > threshold);  /* RT = * */
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
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) { controller = SDL_GameControllerOpen(i); if (controller) break; }
    }
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
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !controller && SDL_IsGameController(e.cdevice.which))
                controller = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                     SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == e.cdevice.which) {
                for (int i = 0; i < 10; i++) if (controller_sources[i]) rt_key_event(0, controller_keys[i]);
                memset(controller_sources, 0, sizeof controller_sources);
                SDL_GameControllerClose(controller); controller = NULL;
            }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP)
                controller_button((SDL_GameControllerButton)e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN);
            else if (e.type == SDL_CONTROLLERAXISMOTION)
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
            char title[192];
            snprintf(title, sizeof title, "Spider-Man: Toxic City | Paint %.1f FPS | Display %.1f FPS | %.2f ms/frame | Target %d | Sleep trim %dms",
                     paint_fps, present_fps, 1000.0 / present_fps, fps, rt_sleep_get_compensation());
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
    if (controller) SDL_GameControllerClose(controller);
    SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    _exit(0);
}
