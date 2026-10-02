/* synth.c - SMF parsing and a compact subtractive synthesiser.
 * Timbres are chosen per General MIDI program family (piano, organ, bass, strings, brass,
 * leads, pads...) and channel 10 is rendered as synthesised drums. It is not a sample-based
 * GM synth, but it reproduces the arrangement (notes, timing, tempo, volume, pan, bend). */
#include "synth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { uint64_t t; uint8_t st, a, b; } MEv;
struct MidiSong { MEv *ev; int n; uint64_t end; int rate; };

typedef struct { uint64_t tick; uint32_t us; } Tempo;
typedef struct { uint64_t tick; uint8_t st, a, b; uint32_t idx; } RawEv;

static int cmp_raw(const void *x, const void *y) {
    const RawEv *a = x, *b = y;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}
static int cmp_tempo(const void *x, const void *y) {
    const Tempo *a = x, *b = y; return a->tick < b->tick ? -1 : a->tick > b->tick;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

MidiSong *midi_parse(const uint8_t *d, int len, int rate) {
    if (len < 14 || memcmp(d, "MThd", 4)) return NULL;
    uint32_t hl = be32(d + 4); if (hl < 6 || (int)hl + 8 > len) return NULL;
    int ntr = be16(d + 10), div = be16(d + 12); int p = 8 + (int)hl;
    RawEv *ev = NULL; int nev = 0, cap = 0; Tempo *tm = NULL; int ntm = 0, tcap = 0; uint32_t idx = 0;
    for (int t = 0; t < ntr && p + 8 <= len; t++) {
        if (memcmp(d + p, "MTrk", 4)) break;
        uint32_t tl = be32(d + p + 4); int q = p + 8, end = q + (int)tl; if (end > len) end = len; p = end;
        uint64_t tick = 0; int running = 0;
        while (q < end) {
            uint32_t dt = 0; int c;
            do { if (q >= end) goto track_done; c = d[q++]; dt = (dt << 7) | (c & 0x7F); } while (c & 0x80);
            tick += dt; if (q >= end) break;
            int st = d[q];
            if (st < 0x80) { if (!running) break; st = running; } else q++;
            if (st == 0xFF) {
                if (q >= end) break;
                int type = d[q++]; uint32_t ml = 0;
                do { if (q >= end) goto track_done; c = d[q++]; ml = (ml << 7) | (c & 0x7F); } while (c & 0x80);
                if (type == 0x51 && ml >= 3 && q + 3 <= end) {
                    if (ntm == tcap) { tcap = tcap ? tcap * 2 : 16; tm = realloc(tm, sizeof(Tempo) * tcap); }
                    tm[ntm].tick = tick; tm[ntm++].us = (uint32_t)d[q] << 16 | d[q + 1] << 8 | d[q + 2];
                }
                q += ml; if (type == 0x2F) break; continue;
            }
            if (st == 0xF0 || st == 0xF7) {
                uint32_t sl = 0; do { if (q >= end) goto track_done; c = d[q++]; sl = (sl << 7) | (c & 0x7F); } while (c & 0x80);
                q += sl; running = 0; continue;
            }
            running = st; int hi = st & 0xF0, need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            if (q + need > end) break;
            if (nev == cap) { cap = cap ? cap * 2 : 1024; ev = realloc(ev, sizeof(RawEv) * cap); }
            ev[nev++] = (RawEv){ tick, (uint8_t)st, d[q], need == 2 ? d[q + 1] : 0, idx++ }; q += need;
        }
    track_done:;
    }
    qsort(ev, nev, sizeof *ev, cmp_raw); qsort(tm, ntm, sizeof *tm, cmp_tempo);
    MidiSong *s = calloc(1, sizeof *s); s->ev = malloc(sizeof(MEv) * (nev ? nev : 1)); s->n = nev; s->rate = rate;
    double us = 500000.0, last_s = 0; uint64_t last_t = 0; int ti = 0;
    double smpte = (div & 0x8000) ? (double)(256 - (div >> 8)) * (div & 0xFF) : 0;
    #define ADV(T) (smpte > 0 ? ((double)((T) - last_t) / smpte * rate) : ((double)((T) - last_t) * us / 1e6 / div * rate))
    for (int i = 0; i < nev; i++) {
        while (ti < ntm && tm[ti].tick <= ev[i].tick) { last_s += ADV(tm[ti].tick); last_t = tm[ti].tick; us = tm[ti].us; ti++; }
        double smp = last_s + ADV(ev[i].tick);
        s->ev[i] = (MEv){ (uint64_t)smp, ev[i].st, ev[i].a, ev[i].b };
    }
    s->end = nev ? s->ev[nev - 1].t : 0;
    free(ev); free(tm); return s;
}
void midi_song_free(MidiSong *s) { if (s) { free(s->ev); free(s); } }
double midi_song_seconds(const MidiSong *s) { return (double)s->end / s->rate; }

/* ------------------------------------------------------------------ synth */
enum { W_SAW, W_SQUARE, W_PULSE, W_TRI, W_SINE };
typedef struct { int wave; float a, d, s, r, gain; } Timbre;
static const Timbre fam[16] = {
    { W_TRI, .002f, 1.3f, .15f, .25f, 1.0f },    /* 0 piano */
    { W_SINE, .001f, .6f, 0, .3f, 1.0f },        /* 1 chromatic percussion */
    { W_SQUARE, .005f, 0, 1.f, .05f, .55f },     /* 2 organ */
    { W_TRI, .002f, .9f, .2f, .15f, 1.0f },      /* 3 guitar */
    { W_PULSE, .003f, .3f, .7f, .08f, .9f },     /* 4 bass */
    { W_SAW, .08f, .2f, .8f, .25f, .45f },       /* 5 strings */
    { W_SAW, .05f, .2f, .8f, .25f, .45f },       /* 6 ensemble */
    { W_SAW, .03f, .1f, .85f, .1f, .5f },        /* 7 brass */
    { W_SQUARE, .03f, .1f, .8f, .1f, .5f },      /* 8 reed */
    { W_SINE, .04f, 0, 1.f, .1f, .9f },          /* 9 pipe */
    { W_SQUARE, .005f, .1f, .8f, .08f, .5f },    /* 10 synth lead */
    { W_SAW, .15f, .3f, .7f, .4f, .4f },         /* 11 synth pad */
    { W_TRI, .1f, .5f, .5f, .3f, .8f },          /* 12 synth fx */
    { W_TRI, .002f, .6f, .2f, .2f, 1.0f },       /* 13 ethnic */
    { W_SINE, .001f, .25f, 0, .1f, 1.0f },       /* 14 percussive */
    { W_SAW, .01f, .5f, .3f, .2f, .4f },         /* 15 sound effects */
};
enum { ST_ATT, ST_DEC, ST_SUS, ST_REL };
enum { D_KICK, D_SNARE, D_HATC, D_HATO, D_CYM, D_TOM, D_CLICK };
typedef struct {
    int active, ch, note, stage, held, drum, dtype; uint32_t age;
    double phase, inc, level, f0; float vel, sus, att_i, dec_i, rel_c, gain, tbuf, pw; int wave;
} Voice;
typedef struct { uint8_t prog, vol, expr, pan, sustain; int bend; double ratio; } Chan;
#define NVOICE 28
struct MidiPlayer { const MidiSong *song; int rate, ei; uint64_t pos; Chan ch[16]; Voice v[NVOICE]; uint32_t age, rng; };

static void reset_channels(MidiPlayer *m) {
    for (int i = 0; i < 16; i++) m->ch[i] = (Chan){ 0, 100, 127, 64, 0, 8192, 1.0 };
}
MidiPlayer *midi_player_new(const MidiSong *s, int rate) {
    MidiPlayer *m = calloc(1, sizeof *m); m->song = s; m->rate = rate; m->rng = 0x9E3779B9u; reset_channels(m); return m;
}
void midi_player_free(MidiPlayer *p) { free(p); }
void midi_rewind(MidiPlayer *m) { memset(m->v, 0, sizeof m->v); m->ei = 0; m->pos = 0; reset_channels(m); }
static void voice_release(MidiPlayer *m, Voice *v) { v->stage = ST_REL; v->held = 0; (void)m; }
void midi_release_all(MidiPlayer *m) { for (int i = 0; i < NVOICE; i++) if (m->v[i].active && m->v[i].stage != ST_REL) voice_release(m, &m->v[i]); }

static void note_on(MidiPlayer *m, int ch, int note, int vel) {
    Voice *v = NULL;
    for (int i = 0; i < NVOICE; i++) if (!m->v[i].active) { v = &m->v[i]; break; }
    if (!v) { uint32_t best = UINT32_MAX; for (int i = 0; i < NVOICE; i++) if (m->v[i].age < best) { best = m->v[i].age; v = &m->v[i]; } }
    memset(v, 0, sizeof *v); v->active = 1; v->ch = ch; v->note = note; v->vel = vel / 127.0f; v->age = ++m->age;
    double rate = m->rate;
    if (ch == 9) {
        v->drum = 1; v->level = 1; v->gain = 0.9f;
        switch (note) {
        case 35: case 36: v->dtype = D_KICK; v->f0 = 130; v->dec_i = 1.f / (0.14f * (float)rate); break;
        case 38: case 40: v->dtype = D_SNARE; v->f0 = 190; v->dec_i = 1.f / (0.16f * (float)rate); break;
        case 42: case 44: v->dtype = D_HATC; v->dec_i = 1.f / (0.045f * (float)rate); break;
        case 46: v->dtype = D_HATO; v->dec_i = 1.f / (0.22f * (float)rate); break;
        case 49: case 51: case 52: case 53: case 55: case 57: case 59: v->dtype = D_CYM; v->dec_i = 1.f / (0.7f * (float)rate); break;
        case 41: case 43: case 45: case 47: case 48: case 50:
            v->dtype = D_TOM; v->f0 = 70 + (note - 41) * 14; v->dec_i = 1.f / (0.26f * (float)rate); break;
        default: v->dtype = D_CLICK; v->dec_i = 1.f / (0.08f * (float)rate); break;
        }
        return;
    }
    const Timbre *t = &fam[m->ch[ch].prog >> 3];
    v->wave = t->wave; v->gain = t->gain; v->sus = t->s; v->pw = t->wave == W_PULSE ? 0.25f : 0.5f;
    v->att_i = 1.f / (t->a * (float)rate + 1.f);
    v->dec_i = t->d > 0 ? (1.f - t->s) / (t->d * (float)rate) : 0.f;
    v->rel_c = expf(-1.f / (t->r * (float)rate * 0.25f + 1.f));
    v->inc = 440.0 * pow(2.0, (note - 69) / 12.0) / rate;
}
static void note_off(MidiPlayer *m, int ch, int note) {
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &m->v[i];
        if (v->active && v->ch == ch && v->note == note && v->stage != ST_REL && !v->drum) {
            if (m->ch[ch].sustain) v->held = 1; else voice_release(m, v);
        }
    }
}
static void handle(MidiPlayer *m, const MEv *e) {
    int ch = e->st & 15, hi = e->st & 0xF0; Chan *c = &m->ch[ch];
    switch (hi) {
    case 0x90: if (e->b) note_on(m, ch, e->a, e->b); else note_off(m, ch, e->a); break;   /* velocity 0 = note off */
    case 0x80: note_off(m, ch, e->a); break;
    case 0xC0: c->prog = e->a; break;
    case 0xE0: c->bend = e->a | (e->b << 7); c->ratio = pow(2.0, ((c->bend - 8192) / 8192.0 * 2.0) / 12.0); break;
    case 0xB0:
        switch (e->a) {
        case 7: c->vol = e->b; break;
        case 10: c->pan = e->b; break;
        case 11: c->expr = e->b; break;
        case 64:
            c->sustain = e->b >= 64;
            if (!c->sustain) for (int i = 0; i < NVOICE; i++) if (m->v[i].active && m->v[i].ch == ch && m->v[i].held) voice_release(m, &m->v[i]);
            break;
        case 120: for (int i = 0; i < NVOICE; i++) if (m->v[i].ch == ch) m->v[i].active = 0; break;
        case 123: for (int i = 0; i < NVOICE; i++) if (m->v[i].active && m->v[i].ch == ch && !m->v[i].drum) voice_release(m, &m->v[i]); break;
        case 121: c->vol = 100; c->expr = 127; c->pan = 64; c->sustain = 0; c->bend = 8192; c->ratio = 1.0; break;
        }
        break;
    }
}
static inline float polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return (float)(t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float)(t * t + t + t + 1.0); }
    return 0.f;
}
static inline float rnd(MidiPlayer *m) { m->rng ^= m->rng << 13; m->rng ^= m->rng >> 17; m->rng ^= m->rng << 5; return (float)(int32_t)m->rng / 2147483648.0f; }

static void render_voices(MidiPlayer *m, float *out, int n) {
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &m->v[i]; if (!v->active) continue;
        Chan *c = &m->ch[v->ch];
        float amp = v->vel * (c->vol / 127.f) * (c->expr / 127.f); amp = amp * amp; /* perceptual curve */
        float pan = c->pan / 127.f, gl = cosf(pan * 1.5707963f), gr = sinf(pan * 1.5707963f);
        float g = amp * v->gain * 0.22f;
        for (int k = 0; k < n; k++) {
            float s;
            if (v->drum) {
                float e = (float)v->level; v->level -= v->dec_i; if (v->level <= 0) { v->active = 0; break; }
                e *= e;
                switch (v->dtype) {
                case D_KICK: { double f = 48 + (v->f0 - 48) * e; v->phase += f / m->rate; s = sinf((float)(v->phase * 6.2831853)) * e * 1.6f; break; }
                case D_TOM: { double f = v->f0 * (0.7 + 0.3 * e); v->phase += f / m->rate; s = sinf((float)(v->phase * 6.2831853)) * e * 1.3f; break; }
                case D_SNARE: { v->phase += v->f0 / m->rate; s = (rnd(m) * 0.8f + sinf((float)(v->phase * 6.2831853)) * 0.5f) * e; break; }
                case D_HATC: case D_HATO: case D_CYM: { float nz = rnd(m); s = (nz - v->tbuf) * 0.6f * e; v->tbuf = nz; break; }
                default: s = rnd(m) * e * 0.6f; break;
                }
            } else {
                switch (v->stage) {
                case ST_ATT: v->level += v->att_i; if (v->level >= 1.0) { v->level = 1.0; v->stage = ST_DEC; } break;
                case ST_DEC: v->level -= v->dec_i; if (v->level <= v->sus) { v->level = v->sus; v->stage = ST_SUS; } break;
                case ST_SUS: break;
                default: v->level *= v->rel_c; if (v->level < 0.0008) { v->active = 0; } break;
                }
                if (!v->active) break;
                double inc = v->inc * c->ratio; v->phase += inc; if (v->phase >= 1.0) v->phase -= 1.0;
                double ph = v->phase;
                switch (v->wave) {
                case W_SAW: s = (float)(2.0 * ph - 1.0) - polyblep(ph, inc); break;
                case W_SQUARE: case W_PULSE: {
                    double pw = v->pw; s = ph < pw ? 1.f : -1.f; s += polyblep(ph, inc);
                    double p2 = ph - pw; if (p2 < 0) p2 += 1.0; s -= polyblep(p2, inc); break; }
                case W_TRI: s = (float)(ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph); break;
                default: s = sinf((float)(ph * 6.2831853)); break;
                }
                s *= (float)v->level;
            }
            out[2 * k] += s * g * gl * 1.4142f; out[2 * k + 1] += s * g * gr * 1.4142f;
        }
    }
}
int midi_render(MidiPlayer *m, float *out, int frames) {
    int done = 0;
    while (done < frames) {
        int n = frames - done; uint64_t next = m->ei < m->song->n ? m->song->ev[m->ei].t : UINT64_MAX;
        if (next <= m->pos) n = 0; else if (next - m->pos < (uint64_t)n) n = (int)(next - m->pos);
        if (n > 0) { render_voices(m, out + 2 * done, n); m->pos += n; done += n; }
        while (m->ei < m->song->n && m->song->ev[m->ei].t <= m->pos) handle(m, &m->song->ev[m->ei++]);
        if (m->ei >= m->song->n && n == 0 && done >= frames) break;
    }
    if (m->ei < m->song->n) return 1;
    for (int i = 0; i < NVOICE; i++) if (m->v[i].active) return 1;
    return 0;
}
