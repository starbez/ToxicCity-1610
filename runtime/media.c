/* media.c - javax.microedition.media: Manager, Player (WAV + MIDI), VolumeControl, and the mixer.
 *
 * The mixer is pull-based: rt_audio_mix() is called by the SDL audio callback, or by a
 * real-time "virtual device" thread in headless runs (optionally recording a WAV file).
 * Player state follows MIDP: UNREALIZED 100, REALIZED 200, PREFETCHED 300, STARTED 400, CLOSED 0.
 * When playback reaches the end the player returns to PREFETCHED and rewinds, as handsets do. */
#include "rt.h"
#include "synth.h"
#include <time.h>
#include <unistd.h>

enum { P_CLOSED = 0, P_UNREALIZED = 100, P_REALIZED = 200, P_PREFETCHED = 300, P_STARTED = 400 };
enum { K_WAV, K_MIDI };
typedef struct JPlayer JPlayer;
typedef struct JVolume { JObj o; JPlayer *p; } JVolume;
struct JPlayer {
    JObj o; int kind, state, loops, remaining, level; JVolume *vol;
    /* WAV: mono float samples at src_rate */
    float *pcm; int npcm; int src_rate; double wpos;
    /* MIDI */
    MidiSong *song; MidiPlayer *mp;
    JPlayer *next_active; int active;
};
JClass rt_VolumeControl_class = { .name = "javax/microedition/media/control/VolumeControl", .super = &rt_Object_class, .size = sizeof(JVolume), .init_state = 2 };
JClass rt_Player_class = { .name = "javax/microedition/media/Player", .super = &rt_Object_class, .size = sizeof(JPlayer), .init_state = 2 };
static const char *const player_ifaces[] = { "javax/microedition/media/Controllable" };
static void __attribute__((constructor)) media_ctor(void) { rt_Player_class.ext_ifaces = player_ifaces; rt_Player_class.next_ifaces = 1; }

static pthread_mutex_t mix_mu = PTHREAD_MUTEX_INITIALIZER;
static JPlayer *active_head;
static float master_gain = 0.8f;

#define P(o) ((JPlayer *)NN(o))
static void throw_media(const char *m) { rt_throw(&rt_MediaException_class, m); }

/* ------------------------------------------------------------- decoding */
static int load_wav(JPlayer *p, const uint8_t *d, int len) {
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    int tag = 0, ch = 0, rate = 0, bits = 0; const uint8_t *data = NULL; int dlen = 0;
    for (int q = 12; q + 8 <= len;) {
        uint32_t cs = d[q + 4] | d[q + 5] << 8 | d[q + 6] << 16 | (uint32_t)d[q + 7] << 24;
        if (!memcmp(d + q, "fmt ", 4) && q + 24 <= len) { tag = d[q + 8] | d[q + 9] << 8; ch = d[q + 10] | d[q + 11] << 8; rate = d[q + 12] | d[q + 13] << 8 | d[q + 14] << 16 | d[q + 15] << 24; bits = d[q + 22] | d[q + 23] << 8; }
        else if (!memcmp(d + q, "data", 4)) { data = d + q + 8; dlen = (int)cs; if (q + 8 + dlen > len) dlen = len - q - 8; }
        q += 8 + (int)cs + (cs & 1);
    }
    if (tag != 1 || !data || ch < 1 || ch > 2 || (bits != 8 && bits != 16) || rate <= 0) return 0;
    int bps = bits / 8, frames = dlen / (bps * ch);
    p->pcm = jvm_alloc(sizeof(float) * (size_t)(frames ? frames : 1)); p->npcm = frames; p->src_rate = rate;
    for (int i = 0; i < frames; i++) {
        float acc = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *s = data + ((size_t)i * ch + c) * bps;
            acc += bits == 8 ? ((int)s[0] - 128) / 128.f : (int16_t)(s[0] | s[1] << 8) / 32768.f;
        }
        p->pcm[i] = acc / ch;
    }
    return 1;
}

JObj *MEDIA(Manager_createPlayer__Ljava_io_InputStream_Ljava_lang_String)(JObj *in, JObj *mime) {
    NN(in); NN(mime); rt_count("media.createPlayer");
    int32_t cap = 1 << 16, n = 0, r; uint8_t *buf = jvm_alloc((size_t)cap);
    JObj *tmp = jvm_newarray(JA_BYTE, 4096);
    while ((r = JIO(InputStream_read__AB)(in, tmp)) > 0) {
        if (n + r > cap) { cap *= 2; buf = realloc(buf, (size_t)cap); }
        memcpy(buf + n, ((JArray *)tmp)->data, (size_t)r); n += r;
    }
    JPlayer *p = (JPlayer *)rt_alloc_obj(&rt_Player_class);
    if (n >= 4 && !memcmp(buf, "MThd", 4)) {
        p->kind = K_MIDI; p->song = midi_parse(buf, n, RT_AUDIO_RATE);
        if (!p->song) throw_media("bad MIDI data");
        p->mp = midi_player_new(p->song, RT_AUDIO_RATE);
    } else if (n >= 4 && !memcmp(buf, "RIFF", 4)) {
        p->kind = K_WAV; if (!load_wav(p, buf, n)) throw_media("unsupported WAV format");
    } else throw_media("unsupported media type");
    free(buf);
    p->state = P_UNREALIZED; p->loops = 1; p->remaining = 1; p->level = 100;
    p->vol = (JVolume *)rt_alloc_obj(&rt_VolumeControl_class); p->vol->p = p;
    return &p->o;
}

/* ---------------------------------------------------------------- mixer */
static void deactivate(JPlayer *p) {      /* caller holds mix_mu */
    for (JPlayer **pp = &active_head; *pp; pp = &(*pp)->next_active) if (*pp == p) { *pp = p->next_active; break; }
    p->active = 0; p->next_active = NULL;
}
static void rewind_player(JPlayer *p) {
    p->wpos = 0; if (p->mp) midi_rewind(p->mp);
}
/* returns 1 while still playing */
static int render_player(JPlayer *p, float *out, int frames) {
    float g = (p->level / 100.f); g *= g * 0.5f + 0.5f;          /* gentle volume curve */
    float tmpbuf[2 * 1024];
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames; int alive;
        if (p->kind == K_MIDI) {
            memset(tmpbuf, 0, sizeof(float) * 2 * (size_t)n); alive = midi_render(p->mp, tmpbuf, n);
            for (int i = 0; i < 2 * n; i++) out[i] += tmpbuf[i] * g;
        } else {
            double step = (double)p->src_rate / RT_AUDIO_RATE; alive = 1;
            for (int i = 0; i < n; i++) {
                int i0 = (int)p->wpos; if (i0 >= p->npcm - 1) { alive = 0; n = i; break; }
                float f = (float)(p->wpos - i0), s = p->pcm[i0] * (1 - f) + p->pcm[i0 + 1] * f;
                out[2 * i] += s * g * 0.9f; out[2 * i + 1] += s * g * 0.9f; p->wpos += step;
            }
        }
        out += 2 * n; frames -= n;
        if (!alive) {
            if (p->loops == -1 || --p->remaining > 0) { rewind_player(p); if (n == 0 && p->kind == K_WAV && p->npcm < 2) return 0; continue; }
            rewind_player(p); return 0;
        }
    }
    return 1;
}
void rt_audio_mix(int16_t *out, int frames) {
    static float *fbuf; static int fcap;
    pthread_mutex_lock(&mix_mu);
    if (frames > fcap) { fbuf = realloc(fbuf, sizeof(float) * 2 * (size_t)frames); fcap = frames; }
    memset(fbuf, 0, sizeof(float) * 2 * (size_t)frames);
    for (JPlayer *p = active_head, *nx; p; p = nx) {
        nx = p->next_active;
        if (!render_player(p, fbuf, frames)) { deactivate(p); if (p->state == P_STARTED) p->state = P_PREFETCHED; }
    }
    pthread_mutex_unlock(&mix_mu);
    for (int i = 0; i < 2 * frames; i++) {
        float x = fbuf[i] * master_gain;
        x = x / (1.f + fabsf(x) * 0.6f) * 1.6f;                  /* soft limiter: no hard clipping */
        if (x > 1.f) x = 1.f; else if (x < -1.f) x = -1.f;
        out[i] = (int16_t)(x * 32767.f);
    }
}

/* --- headless "virtual audio device": pulls the mixer in real time, optionally records a WAV --- */
static FILE *wavf; static uint32_t wav_bytes; static volatile int vdev_run;
static void wav_header(FILE *f, uint32_t data) {
    uint8_t h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0 };
    uint32_t r = RT_AUDIO_RATE, br = RT_AUDIO_RATE * 4, ds = data, rs = data + 36;
    memcpy(h + 4, &rs, 4); memcpy(h + 24, &r, 4); memcpy(h + 28, &br, 4); h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4); memcpy(h + 40, &ds, 4); fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f);
}
static void *vdev_thread(void *arg) {
    (void)arg; int16_t buf[2 * 441]; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    while (vdev_run) {
        rt_audio_mix(buf, 441);
        if (wavf) { fwrite(buf, 4, 441, wavf); wav_bytes += 441 * 4; }
        t.tv_nsec += 10000000L; if (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    }
    return NULL;
}
void rt_audio_virtual_start(const char *wavpath) {
    if (wavpath) { wavf = fopen(wavpath, "wb"); if (wavf) wav_header(wavf, 0); }
    vdev_run = 1; pthread_t th; pthread_create(&th, NULL, vdev_thread, NULL); pthread_detach(th);
}
void rt_audio_virtual_stop(void) {
    vdev_run = 0; usleep(30000);
    if (wavf) { wav_header(wavf, wav_bytes); fclose(wavf); wavf = NULL; }
}

/* ----------------------------------------------------------- Player API */
void MEDIA(Player_realize__)(JObj *s)  { JPlayer *p = P(s); if (p->state == P_CLOSED) rt_throw(&rt_IllegalStateException_class, NULL); if (p->state < P_REALIZED) p->state = P_REALIZED; }
void MEDIA(Player_prefetch__)(JObj *s) { JPlayer *p = P(s); if (p->state == P_CLOSED) rt_throw(&rt_IllegalStateException_class, NULL); if (p->state < P_PREFETCHED) p->state = P_PREFETCHED; }
void MEDIA(Player_start__)(JObj *s) {
    JPlayer *p = P(s); if (p->state == P_CLOSED) rt_throw(&rt_IllegalStateException_class, NULL);
    pthread_mutex_lock(&mix_mu);
    if (p->state != P_STARTED) {
        p->state = P_STARTED; p->remaining = p->loops;
        if (!p->active) { p->next_active = active_head; active_head = p; p->active = 1; }
    }
    pthread_mutex_unlock(&mix_mu); rt_count("media.start");
}
void MEDIA(Player_stop__)(JObj *s) {
    JPlayer *p = P(s); pthread_mutex_lock(&mix_mu);
    if (p->state == P_STARTED) {
        p->state = P_PREFETCHED; deactivate(p);
        if (p->mp) midi_release_all(p->mp);                       /* MIDI keeps its position (pause) */
    }
    pthread_mutex_unlock(&mix_mu);
}
void MEDIA(Player_close__)(JObj *s) {
    JPlayer *p = P(s); pthread_mutex_lock(&mix_mu);
    if (p->active) deactivate(p);
    p->state = P_CLOSED; pthread_mutex_unlock(&mix_mu);
}
int32_t MEDIA(Player_getState__)(JObj *s) { return P(s)->state; }
void MEDIA(Player_setLoopCount__I)(JObj *s, int32_t n) {
    if (n == 0) rt_throw(&rt_IllegalArgumentException_class, NULL);
    JPlayer *p = P(s); if (p->state == P_STARTED) rt_throw(&rt_IllegalStateException_class, NULL);
    p->loops = n; p->remaining = n;
}
int64_t MEDIA(Player_setMediaTime__J)(JObj *s, int64_t t) {
    JPlayer *p = P(s); pthread_mutex_lock(&mix_mu);
    if (t <= 0) rewind_player(p);
    pthread_mutex_unlock(&mix_mu); return t < 0 ? 0 : t;
}
JObj *MEDIA(Controllable_getControl__Ljava_lang_String)(JObj *s, JObj *name) {
    JPlayer *p = P(s); char *n = jstr_dup_utf8(NN(name)); int vc = strstr(n, "VolumeControl") != NULL; free(n);
    return vc ? &p->vol->o : NULL;
}
int32_t MCTL(VolumeControl_setLevel__I)(JObj *s, int32_t l) {
    JVolume *v = (JVolume *)NN(s); l = l < 0 ? 0 : l > 100 ? 100 : l; v->p->level = l; return l;
}
