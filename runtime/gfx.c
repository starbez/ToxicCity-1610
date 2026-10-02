/* gfx.c - software renderer: javax.microedition.lcdui.Image and Graphics (ARGB32 surfaces).
 *
 * Semantics follow MIDP 2.0 (JSR-118): clip rectangle always applies, drawRect covers
 * x..x+w / y..y+h inclusive, anchors are validated, drawRegion implements the 8 sprite
 * transforms, and alpha is blended source-over onto an opaque destination. */
#include "gfx.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#include <stdarg.h>
#include <stdatomic.h>

JClass rt_Image_class = { .name = "javax/microedition/lcdui/Image", .super = &rt_Object_class, .size = sizeof(JImage), .init_state = 2 };
JClass rt_Graphics_class = { .name = "javax/microedition/lcdui/Graphics", .super = &rt_Object_class, .size = sizeof(JGraphics), .init_state = 2 };

enum { A_HCENTER = 1, A_VCENTER = 2, A_LEFT = 4, A_RIGHT = 8, A_TOP = 16, A_BOTTOM = 32, A_BASELINE = 64 };

#define CNT(name) rt_count("lcdui." name)
#define G(o) ((JGraphics *)NN(o))
#define IAE() rt_throw(&rt_IllegalArgumentException_class, __func__)

/* Optional decoded-art export and same-size PNG replacements for modding. */
static _Atomic uint32_t texture_serial;
static pthread_mutex_t texture_index_mu = PTHREAD_MUTEX_INITIALIZER;
static int texture_index_started;
static void texture_log(const char *fmt, ...) {
    FILE *f = fopen("texture_override.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}
static uint32_t texture_hash_pixels(int32_t w, int32_t h, const uint32_t *pix) {
    uint32_t hash = 2166136261u;
    const uint32_t dims[2] = { (uint32_t)w, (uint32_t)h };
    for (int n = 0; n < 2; n++) for (int b = 0; b < 4; b++) { hash ^= (dims[n] >> (8 * b)) & 255; hash *= 16777619u; }
    for (size_t k = 0, count = (size_t)w * h; k < count; k++)
        for (int b = 0; b < 4; b++) { hash ^= (pix[k] >> (8 * b)) & 255; hash *= 16777619u; }
    return hash;
}
static uint32_t texture_hash(const JImage *im) { return texture_hash_pixels(im->w, im->h, im->pix); }
static uint32_t texture_hash_rgba(int32_t w, int32_t h, const stbi_uc *rgba) {
    size_t count = (size_t)w * h; uint32_t *pixels = malloc(count * sizeof(uint32_t));
    if (!pixels) return 0;
    for (size_t k = 0; k < count; k++)
        pixels[k] = ((uint32_t)rgba[k * 4 + 3] << 24) | ((uint32_t)rgba[k * 4] << 16) |
                    ((uint32_t)rgba[k * 4 + 1] << 8) | rgba[k * 4 + 2];
    uint32_t hash = texture_hash_pixels(w, h, pixels); free(pixels); return hash;
}
static stbi_uc *texture_load_png(const char *path, int *w, int *h, int *comp) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    stbi_uc *result = NULL;
    if (n > 0 && n <= INT_MAX) {
        stbi_uc *bytes = malloc((size_t)n);
        if (bytes && fread(bytes, 1, (size_t)n, f) == (size_t)n)
            result = stbi_load_from_memory(bytes, (int)n, w, h, comp, 4);
        free(bytes);
    }
    fclose(f); return result;
}
static stbi_uc *texture_downsample(const stbi_uc *src, int sw, int sh, int dw, int dh) {
    if (sw < dw || sh < dh || sw % dw || sh % dh || sw / dw != sh / dh) return NULL;
    int factor = sw / dw; size_t count = (size_t)dw * dh;
    stbi_uc *out = malloc(count * 4); if (!out) return NULL;
    int area = factor * factor;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        uint32_t sum[4] = { 0, 0, 0, 0 };
        for (int yy = 0; yy < factor; yy++) for (int xx = 0; xx < factor; xx++) {
            const stbi_uc *p = src + ((size_t)(y * factor + yy) * sw + x * factor + xx) * 4;
            for (int c = 0; c < 4; c++) sum[c] += p[c];
        }
        stbi_uc *p = out + ((size_t)y * dw + x) * 4;
        for (int c = 0; c < 4; c++) p[c] = (stbi_uc)((sum[c] + area / 2) / area);
    }
    return out;
}
static void texture_tools(uint32_t id, JImage *im) {
    const char *dump = getenv("TOXICCITY_DUMP_TEXTURES");
    const char *mods = getenv("TOXICCITY_TEXTURE_OVERRIDES");
    char path[1024];
    uint32_t hash = texture_hash(im);
    if (id == 0 && mods && *mods) texture_log("\nOverrides: %s\n", mods);
    if (dump && *dump) {
        pthread_mutex_lock(&texture_index_mu);
        snprintf(path, sizeof path, "%s/texture_index.csv", dump);
        FILE *index = fopen(path, texture_index_started ? "a" : "w");
        if (index) {
            if (!texture_index_started) fputs("load_id,stable_hash,width,height\n", index);
            fprintf(index, "%u,%08x,%d,%d\n", id, hash, im->w, im->h);
            fclose(index); texture_index_started = 1;
        }
        pthread_mutex_unlock(&texture_index_mu);
        size_t count = (size_t)im->w * im->h;
        stbi_uc *rgba = malloc(count * 4);
        if (rgba) {
            for (size_t k = 0; k < count; k++) {
                uint32_t p = im->pix[k];
                rgba[k * 4] = (stbi_uc)(p >> 16); rgba[k * 4 + 1] = (stbi_uc)(p >> 8);
                rgba[k * 4 + 2] = (stbi_uc)p; rgba[k * 4 + 3] = (stbi_uc)(p >> 24);
            }
            snprintf(path, sizeof path, "%s/texture_%04u.png", dump, id);
            stbi_write_png(path, im->w, im->h, 4, rgba, im->w * 4);
            snprintf(path, sizeof path, "%s/texture_%08x.png", dump, hash);
            stbi_write_png(path, im->w, im->h, 4, rgba, im->w * 4);
            free(rgba);
        }
    }
    if (mods && *mods) {
        int w = 0, h = 0, comp = 0, nw = 0, nh = 0, nc = 0;
        char hash_path[1024], numbered_path[1024];
        snprintf(hash_path, sizeof hash_path, "%s/texture_%08x.png", mods, hash);
        snprintf(numbered_path, sizeof numbered_path, "%s/texture_%04u.png", mods, id);
        stbi_uc *rgba = texture_load_png(hash_path, &w, &h, &comp);
        stbi_uc *numbered = texture_load_png(numbered_path, &nw, &nh, &nc);
        if (numbered) {
            if (nw != im->w || nh != im->h || texture_hash_rgba(nw, nh, numbered) != hash || !rgba) {
                stbi_image_free(rgba); rgba = numbered; w = nw; h = nh; comp = nc;
                snprintf(path, sizeof path, "%s", numbered_path);
            } else {
                stbi_image_free(numbered); snprintf(path, sizeof path, "%s", hash_path);
            }
        } else if (rgba) snprintf(path, sizeof path, "%s", hash_path);
        if (rgba && (w != im->w || h != im->h)) {
            stbi_uc *scaled = texture_downsample(rgba, w, h, im->w, im->h);
            if (scaled) {
                texture_log("Downsampled %s from %dx%d to %dx%d\n", path, w, h, im->w, im->h);
                stbi_image_free(rgba); rgba = scaled; w = im->w; h = im->h;
            }
        }
        if (rgba) {
            if (w == im->w && h == im->h) {
                for (size_t k = 0; k < (size_t)w * h; k++)
                    im->pix[k] = ((uint32_t)rgba[k * 4 + 3] << 24) | ((uint32_t)rgba[k * 4] << 16) |
                                 ((uint32_t)rgba[k * 4 + 1] << 8) | rgba[k * 4 + 2];
                texture_log("Applied %s (%dx%d)\n", path, w, h);
            } else {
                fprintf(stderr, "[texture] %s is %dx%d; expected %dx%d, skipped\n", path, w, h, im->w, im->h);
                texture_log("Skipped %s: got %dx%d, expected %dx%d\n", path, w, h, im->w, im->h);
            }
            stbi_image_free(rgba);
        } else texture_log("No usable replacement: %s\n", path);
    }
}

/* ------------------------------------------------------------------ pixels */
static inline uint32_t blend(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 255) return src;
    if (a == 0) return dst;
    uint32_t ia = 255 - a, out = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t s = (src >> sh) & 255, d = (dst >> sh) & 255, v = s * a + d * ia + 128;
        out |= (((v + (v >> 8)) >> 8) & 255) << sh;
    }
    return out;
}

typedef struct Rect { int32_t x0, y0, x1, y1; } Rect;   /* half-open */
static Rect clip_of(JGraphics *g) {
    Rect r = { g->cx, g->cy, g->cx + g->cw, g->cy + g->ch };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > g->img->w) r.x1 = g->img->w;
    if (r.y1 > g->img->h) r.y1 = g->img->h;
    return r;
}
static inline void put(JGraphics *g, const Rect *c, int32_t x, int32_t y, uint32_t argb) {
    if (x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1) g->img->pix[(size_t)y * g->img->w + x] = argb;
}
static void fill_span(JGraphics *g, int32_t x0, int32_t x1, int32_t y, uint32_t argb) {   /* [x0,x1] inclusive */
    Rect c = clip_of(g);
    if (y < c.y0 || y >= c.y1) return;
    if (x0 < c.x0) x0 = c.x0;
    if (x1 >= c.x1) x1 = c.x1 - 1;
    uint32_t *p = g->img->pix + (size_t)y * g->img->w;
    for (int32_t x = x0; x <= x1; x++) p[x] = argb;
}
static inline uint32_t col(JGraphics *g) { return 0xFF000000u | (uint32_t)g->color; }

/* ------------------------------------------------------------------- Image */
static JImage *img_new(int32_t w, int32_t h, int mut) {
    if (w <= 0 || h <= 0) IAE();
    JImage *i = (JImage *)rt_alloc_obj(&rt_Image_class);
    i->w = w; i->h = h; i->mut = mut; i->pix = jvm_alloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
    return i;
}
JObj *LCDUI(Image_createImage__I_I)(int32_t w, int32_t h) {
    CNT("Image.createImage(w,h)");
    JImage *i = img_new(w, h, 1);
    for (size_t k = 0; k < (size_t)w * (size_t)h; k++) i->pix[k] = 0xFFFFFFFFu;
    return &i->o;
}
JObj *LCDUI(Image_createImage__AB_I_I)(JObj *data, int32_t off, int32_t len) {
    CNT("Image.createImage(bytes)");
    JArray *a = (JArray *)NN(data);
    if (off < 0 || len < 0 || off + len > a->len) rt_throw(&rt_ArrayIndexOutOfBoundsException_class, NULL);
    int w, h, comp;
    uint8_t *rgba = stbi_load_from_memory(a->data + off, len, &w, &h, &comp, 4);
    if (!rgba) rt_throw(&rt_IllegalArgumentException_class, "cannot decode image");
    JImage *i = img_new(w, h, 0);
    for (size_t k = 0; k < (size_t)w * (size_t)h; k++)
        i->pix[k] = ((uint32_t)rgba[k * 4 + 3] << 24) | ((uint32_t)rgba[k * 4] << 16) | ((uint32_t)rgba[k * 4 + 1] << 8) | rgba[k * 4 + 2];
    stbi_image_free(rgba);
    texture_tools(atomic_fetch_add(&texture_serial, 1), i);
    return &i->o;
}
JObj *LCDUI(Image_createRGBImage__AI_I_I_Z)(JObj *rgb, int32_t w, int32_t h, int32_t alpha) {
    CNT("Image.createRGBImage");
    JArray *a = (JArray *)NN(rgb);
    if (w <= 0 || h <= 0) IAE();
    if ((int64_t)w * h > a->len) rt_throw(&rt_ArrayIndexOutOfBoundsException_class, NULL);
    JImage *i = img_new(w, h, 0);
    memcpy(i->pix, a->data, sizeof(uint32_t) * (size_t)w * (size_t)h);
    if (!alpha) for (size_t k = 0; k < (size_t)w * (size_t)h; k++) i->pix[k] |= 0xFF000000u;
    texture_tools(atomic_fetch_add(&texture_serial, 1), i);
    return &i->o;
}
int32_t LCDUI(Image_getWidth__)(JObj *self) { return ((JImage *)NN(self))->w; }
int32_t LCDUI(Image_getHeight__)(JObj *self) { return ((JImage *)NN(self))->h; }
void LCDUI(Image_getRGB__AI_I_I_I_I_I_I)(JObj *self, JObj *rgb, int32_t off, int32_t scan, int32_t x, int32_t y, int32_t w, int32_t h) {
    JImage *im = (JImage *)NN(self); JArray *a = (JArray *)NN(rgb);
    CNT("Image.getRGB");
    if (x < 0 || y < 0 || w < 0 || h < 0 || (int64_t)x + w > im->w || (int64_t)y + h > im->h) IAE();
    for (int32_t j = 0; j < h; j++)
        for (int32_t i = 0; i < w; i++) {
            int64_t d = (int64_t)off + (int64_t)j * scan + i;
            if (d < 0 || d >= a->len) jvm_throw_aioobe((int32_t)d);
            ((uint32_t *)a->data)[d] = im->pix[(size_t)(y + j) * im->w + x + i];
        }
}
JGraphics *gfx_new_graphics(JImage *img) {
    JGraphics *g = (JGraphics *)rt_alloc_obj(&rt_Graphics_class);
    g->img = img; g->cw = img->w; g->ch = img->h; return g;
}
JObj *LCDUI(Image_getGraphics__)(JObj *self) {
    JImage *im = (JImage *)NN(self);
    if (!im->mut) rt_throw(&rt_IllegalStateException_class, "immutable image");
    CNT("Image.getGraphics");
    return &gfx_new_graphics(im)->o;
}

/* ---------------------------------------------------------- state / clip */
void LCDUI(Graphics_setColor__I)(JObj *g, int32_t c) { G(g)->color = c & 0xFFFFFF; }
void LCDUI(Graphics_setColor__I_I_I)(JObj *g, int32_t r, int32_t gg, int32_t b) {
    if ((uint32_t)r > 255 || (uint32_t)gg > 255 || (uint32_t)b > 255) IAE();
    G(g)->color = (r << 16) | (gg << 8) | b;
}
void LCDUI(Graphics_setFont__Ljavax_microedition_lcdui_Font)(JObj *g, JObj *f) { G(g)->font = f; }
void LCDUI(Graphics_setClip__I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h) {
    JGraphics *g = G(go);
    /* store the clip clamped to the surface, as MIDP implementations report it back */
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + (w < 0 ? 0 : w), y1 = (int64_t)y + (h < 0 ? 0 : h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->img->w) x1 = g->img->w;
    if (y1 > g->img->h) y1 = g->img->h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    g->cx = (int32_t)x0; g->cy = (int32_t)y0; g->cw = (int32_t)(x1 - x0); g->ch = (int32_t)(y1 - y0);
}
int32_t LCDUI(Graphics_getClipX__)(JObj *g) { return G(g)->cx; }
int32_t LCDUI(Graphics_getClipY__)(JObj *g) { return G(g)->cy; }
int32_t LCDUI(Graphics_getClipWidth__)(JObj *g) { return G(g)->cw; }
int32_t LCDUI(Graphics_getClipHeight__)(JObj *g) { return G(g)->ch; }

/* ------------------------------------------------------------ primitives */
void LCDUI(Graphics_fillRect__I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h) {
    JGraphics *g = G(go); CNT("Graphics.fillRect");
    if (w <= 0 || h <= 0) return;
    Rect c = clip_of(g);
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x0 < c.x0) x0 = c.x0;
    if (y0 < c.y0) y0 = c.y0;
    if (x1 > c.x1) x1 = c.x1;
    if (y1 > c.y1) y1 = c.y1;
    uint32_t v = col(g);
    for (int64_t j = y0; j < y1; j++) {
        uint32_t *p = g->img->pix + (size_t)j * g->img->w;
        for (int64_t i = x0; i < x1; i++) p[i] = v;
    }
}
static void line(JGraphics *g, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    uint32_t v = col(g);
    if (y0 == y1) { if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; } fill_span(g, x0, x1, y0, v); return; }
    Rect c = clip_of(g);
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        put(g, &c, x0, y0, v);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
void LCDUI(Graphics_drawLine__I_I_I_I)(JObj *go, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    CNT("Graphics.drawLine"); line(G(go), x0, y0, x1, y1);
}
void LCDUI(Graphics_drawRect__I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h) {
    JGraphics *g = G(go); CNT("Graphics.drawRect");
    if (w < 0 || h < 0) return;
    line(g, x, y, x + w, y);
    if (h > 0) line(g, x, y + h, x + w, y + h);
    if (h > 1) { line(g, x, y + 1, x, y + h - 1); if (w > 0) line(g, x + w, y + 1, x + w, y + h - 1); }
}
void LCDUI(Graphics_fillTriangle__I_I_I_I_I_I)(JObj *go, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    JGraphics *g = G(go); CNT("Graphics.fillTriangle"); uint32_t v = col(g);
    int32_t ys[3] = { y0, y1, y2 }, xs[3] = { x0, x1, x2 };
    int32_t ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < 3; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    Rect c = clip_of(g); if (ymin < c.y0) ymin = c.y0; if (ymax >= c.y1) ymax = c.y1 - 1;
    for (int32_t y = ymin; y <= ymax; y++) {
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (int e = 0; e < 3; e++) {
            int32_t ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
            if (ay == by) { if (y == ay) { int32_t l = ax < bx ? ax : bx, h = ax < bx ? bx : ax; if (l < lo) lo = l; if (h > hi) hi = h; } continue; }
            if ((y < ay && y < by) || (y > ay && y > by)) continue;
            int64_t xi = ax + ((int64_t)(y - ay) * (bx - ax)) / (by - ay);
            if (xi < lo) lo = (int32_t)xi; if (xi > hi) hi = (int32_t)xi;
        }
        if (lo <= hi) fill_span(g, lo, hi, y, v);
    }
}

/* ellipse-based shapes (angles in degrees, 0 = 3 o'clock, counter-clockwise) */
static int in_sector(double dx, double dy, int32_t start, int32_t sweep) {
    if (sweep >= 360 || sweep <= -360) return 1;
    double ang = atan2(-dy, dx) * 180.0 / M_PI; if (ang < 0) ang += 360.0;
    double s = fmod((double)start, 360.0); if (s < 0) s += 360.0;
    double rel = ang - s; if (rel < 0) rel += 360.0;
    if (sweep >= 0) return rel <= sweep;
    return (360.0 - rel) <= -sweep || rel == 0;
}
void LCDUI(Graphics_fillArc__I_I_I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h, int32_t sa, int32_t aa) {
    JGraphics *g = G(go); CNT("Graphics.fillArc");
    if (w <= 0 || h <= 0 || aa == 0) return;
    double rx = w / 2.0, ry = h / 2.0, cx = x + rx, cy = y + ry; Rect c = clip_of(g); uint32_t v = col(g);
    for (int32_t j = y; j < y + h; j++) for (int32_t i = x; i < x + w; i++) {
        double dx = (i + 0.5 - cx) / rx, dy = (j + 0.5 - cy) / ry;
        if (dx * dx + dy * dy <= 1.0 && in_sector(dx * rx, dy * ry, sa, aa)) put(g, &c, i, j, v);
    }
}
void LCDUI(Graphics_drawArc__I_I_I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h, int32_t sa, int32_t aa) {
    JGraphics *g = G(go); CNT("Graphics.drawArc");
    if (w < 0 || h < 0 || aa == 0) return;
    double rx = w / 2.0, ry = h / 2.0, cx = x + rx, cy = y + ry; Rect c = clip_of(g); uint32_t v = col(g);
    int steps = (int)((rx + ry) * 8) + 16; int32_t sweep = aa > 360 ? 360 : aa < -360 ? -360 : aa;
    for (int k = 0; k <= steps; k++) {
        double a = (sa + (double)sweep * k / steps) * M_PI / 180.0;
        put(g, &c, (int32_t)lround(cx + rx * cos(a)), (int32_t)lround(cy - ry * sin(a)), v);
    }
}
static int rr_inset(int32_t dy_from_edge, int32_t aw, int32_t ah) {   /* x-inset of a rounded corner at a row */
    if (ah <= 0 || aw <= 0 || dy_from_edge >= ah / 2) return 0;
    double ry = ah / 2.0, rx = aw / 2.0, t = (ry - (dy_from_edge + 0.5)) / ry;
    return (int)ceil(rx - rx * sqrt(1.0 - t * t));
}
void LCDUI(Graphics_fillRoundRect__I_I_I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h, int32_t aw, int32_t ah) {
    JGraphics *g = G(go); CNT("Graphics.fillRoundRect"); if (w <= 0 || h <= 0) return;
    for (int32_t j = 0; j < h; j++) {
        int32_t e = j < h - 1 - j ? j : h - 1 - j, in = rr_inset(e, aw, ah);
        fill_span(g, x + in, x + w - 1 - in, y + j, col(g));
    }
}
void LCDUI(Graphics_drawRoundRect__I_I_I_I_I_I)(JObj *go, int32_t x, int32_t y, int32_t w, int32_t h, int32_t aw, int32_t ah) {
    JGraphics *g = G(go); CNT("Graphics.drawRoundRect"); if (w < 0 || h < 0) return;
    Rect c = clip_of(g); uint32_t v = col(g);
    for (int32_t j = 0; j <= h; j++) {
        int32_t e = j < h - j ? j : h - j, in = rr_inset(e, aw, ah);
        put(g, &c, x + in, y + j, v); put(g, &c, x + w - in, y + j, v);
        if (j == 0 || j == h) fill_span(g, x + in, x + w - in, y + j, v);
    }
}

/* ------------------------------------------------------------ image blits */
static void anchor_xy(int32_t *x, int32_t *y, int32_t w, int32_t h, int32_t anchor, int allow_baseline) {
    int hz = anchor & (A_LEFT | A_HCENTER | A_RIGHT), vt = anchor & (A_TOP | A_VCENTER | A_BOTTOM | A_BASELINE);
    /* Real handsets are lenient about a missing half of the anchor (0 acts as TOP|LEFT) and many
     * games rely on that, so only genuinely conflicting or unknown bits are rejected. */
    if (!hz) hz = A_LEFT;
    if (!vt) vt = A_TOP;
    if ((hz != A_LEFT && hz != A_HCENTER && hz != A_RIGHT) || (anchor & ~127)) { fprintf(stderr, "[gfx] bad anchor %d\n", anchor); IAE(); }
    if (vt != A_TOP && vt != A_VCENTER && vt != A_BOTTOM && !(allow_baseline && vt == A_BASELINE)) { fprintf(stderr, "[gfx] bad anchor %d\n", anchor); IAE(); }
    if (hz == A_HCENTER) *x -= w >> 1; else if (hz == A_RIGHT) *x -= w;
    if (vt == A_VCENTER) *y -= h >> 1; else if (vt == A_BOTTOM || vt == A_BASELINE) *y -= h;
}
static void blit(JGraphics *g, const JImage *src, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t transform, int32_t dx, int32_t dy) {
    Rect c = clip_of(g);
    int swap = transform >= 4, tw = swap ? h : w, th = swap ? w : h;
    for (int32_t j = 0; j < th; j++) {
        int32_t py = dy + j; if (py < c.y0 || py >= c.y1) continue;
        for (int32_t i = 0; i < tw; i++) {
            int32_t px = dx + i; if (px < c.x0 || px >= c.x1) continue;
            int32_t u, v;
            switch (transform) {
            case 0: u = i; v = j; break;
            case 1: u = i; v = h - 1 - j; break;               /* MIRROR_ROT180 */
            case 2: u = w - 1 - i; v = j; break;               /* MIRROR */
            case 3: u = w - 1 - i; v = h - 1 - j; break;       /* ROT180 */
            case 4: u = j; v = i; break;                       /* MIRROR_ROT270 */
            case 5: u = j; v = h - 1 - i; break;               /* ROT90 */
            case 6: u = w - 1 - j; v = i; break;               /* ROT270 */
            default: u = w - 1 - j; v = h - 1 - i; break;      /* MIRROR_ROT90 */
            }
            uint32_t s = src->pix[(size_t)(sy + v) * src->w + sx + u];
            uint32_t *d = &g->img->pix[(size_t)py * g->img->w + px];
            *d = blend(*d, s);
        }
    }
}
void LCDUI(Graphics_drawImage__Ljavax_microedition_lcdui_Image_I_I_I)(JObj *go, JObj *io, int32_t x, int32_t y, int32_t anchor) {
    JGraphics *g = G(go); JImage *im = (JImage *)NN(io); CNT("Graphics.drawImage");
    anchor_xy(&x, &y, im->w, im->h, anchor, 0);
    blit(g, im, 0, 0, im->w, im->h, 0, x, y);
}
void LCDUI(Graphics_drawRegion__Ljavax_microedition_lcdui_Image_I_I_I_I_I_I_I_I)(JObj *go, JObj *io, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t transform, int32_t x, int32_t y, int32_t anchor) {
    JGraphics *g = G(go); JImage *im = (JImage *)NN(io); CNT("Graphics.drawRegion");
    if (transform < 0 || transform > 7) IAE();
    if (w < 0 || h < 0 || sx < 0 || sy < 0 || (int64_t)sx + w > im->w || (int64_t)sy + h > im->h) IAE();
    if (w == 0 || h == 0) return;
    int swap = transform >= 4;
    anchor_xy(&x, &y, swap ? h : w, swap ? w : h, anchor, 0);
    blit(g, im, sx, sy, w, h, transform, x, y);
}
void LCDUI(Graphics_drawRGB__AI_I_I_I_I_I_I_Z)(JObj *go, JObj *rgb, int32_t off, int32_t scan, int32_t x, int32_t y, int32_t w, int32_t h, int32_t alpha) {
    JGraphics *g = G(go); JArray *a = (JArray *)NN(rgb); CNT("Graphics.drawRGB");
    if (w <= 0 || h <= 0) return;
    int64_t last = (int64_t)off + (int64_t)(h - 1) * scan + (w - 1), first = off;
    if (scan < 0) { int64_t t = first; first = (int64_t)off + (int64_t)(h - 1) * scan; last = t + (w - 1); }
    if (first < 0 || last >= a->len) jvm_throw_aioobe((int32_t)(first < 0 ? first : last));
    Rect c = clip_of(g); const uint32_t *src = (const uint32_t *)a->data;
    for (int32_t j = 0; j < h; j++) {
        int32_t py = y + j; if (py < c.y0 || py >= c.y1) continue;
        for (int32_t i = 0; i < w; i++) {
            int32_t px = x + i; if (px < c.x0 || px >= c.x1) continue;
            uint32_t s = src[(int64_t)off + (int64_t)j * scan + i]; if (!alpha) s |= 0xFF000000u;
            uint32_t *d = &g->img->pix[(size_t)py * g->img->w + px]; *d = blend(*d, s);
        }
    }
}
/* Text: the game draws its text with its own bitmap fonts through drawRGB/drawRegion.
 * System-font drawString is accepted but not rendered yet (counted, so it shows in the stats). */
void LCDUI(Graphics_drawString__Ljava_lang_String_I_I_I)(JObj *go, JObj *s, int32_t x, int32_t y, int32_t a) {
    (void)x; (void)y; (void)a; G(go); NN(s); CNT("Graphics.drawString(unrendered)");
}
