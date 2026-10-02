/* lang.c - java.lang.{String,StringBuffer,Integer,Math,System,Object,Class,Throwable}, java.io.PrintStream, java.util.Random */
#include "rt.h"
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

/* =============================================================== String */
JObj *jstr_new(const uint16_t *s, int32_t n) {
    JString *r = (JString *)rt_alloc_obj(&rt_String_class);
    r->len = n; r->chars = jvm_alloc(sizeof(uint16_t) * (size_t)(n ? n : 1));
    if (n) memcpy(r->chars, s, sizeof(uint16_t) * (size_t)n);
    return &r->o;
}
static JString *S(JObj *o) { return (JString *)NN(o); }

static int32_t utf8_to_utf16(const uint8_t *b, int32_t n, uint16_t *out) {
    int32_t o = 0;
    for (int32_t i = 0; i < n;) {
        uint32_t c = b[i];
        int need = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (need < 0 || i + need >= n) { out[o++] = 0xFFFD; i++; continue; }   /* invalid / truncated */
        if (need == 0) { out[o++] = (uint16_t)c; i++; continue; }
        c &= (0x3Fu >> need);
        for (int k = 1; k <= need; k++) c = (c << 6) | (b[i + k] & 0x3Fu);
        i += need + 1;
        if (c >= 0x10000) { c -= 0x10000; out[o++] = (uint16_t)(0xD800 + (c >> 10)); out[o++] = (uint16_t)(0xDC00 + (c & 0x3FF)); }
        else out[o++] = (uint16_t)c;
    }
    return o;
}
JObj *jstr_from_utf8(const char *s) {
    int32_t n = (int32_t)strlen(s);
    uint16_t *tmp = jvm_alloc(sizeof(uint16_t) * (size_t)(n + 1));
    int32_t m = utf8_to_utf16((const uint8_t *)s, n, tmp);
    JObj *r = jstr_new(tmp, m); free(tmp); return r;
}
static int enc_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x != y) return 0; }
    return !*a && !*b;
}
JObj *jstr_decode(const uint8_t *b, int32_t n, const char *enc) {
    uint16_t *tmp = jvm_alloc(sizeof(uint16_t) * (size_t)(n + 1)); int32_t m = 0;
    if (enc && (enc_eq(enc, "UTF-8") || enc_eq(enc, "UTF8"))) m = utf8_to_utf16(b, n, tmp);
    else if (enc && (enc_eq(enc, "UTF-16") || enc_eq(enc, "UTF-16BE"))) { for (int32_t i = 0; i + 1 < n; i += 2) tmp[m++] = (uint16_t)((b[i] << 8) | b[i + 1]); }
    else if (enc && enc_eq(enc, "UTF-16LE")) { for (int32_t i = 0; i + 1 < n; i += 2) tmp[m++] = (uint16_t)((b[i + 1] << 8) | b[i]); }
    else { for (int32_t i = 0; i < n; i++) tmp[m++] = b[i]; }   /* ISO-8859-1 (also the default) */
    JObj *r = jstr_new(tmp, m); free(tmp); return r;
}
char *jstr_dup_utf8(JObj *o) {
    if (!o) return NULL;
    JString *s = (JString *)o; char *r = jvm_alloc((size_t)s->len * 3 + 1); size_t k = 0;
    for (int32_t i = 0; i < s->len; i++) {
        uint32_t c = s->chars[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->chars[i + 1] >= 0xDC00 && s->chars[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->chars[++i] - 0xDC00);
        }
        if (c < 0x80) r[k++] = (char)c;
        else if (c < 0x800) { r[k++] = (char)(0xC0 | (c >> 6)); r[k++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { r[k++] = (char)(0xE0 | (c >> 12)); r[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); r[k++] = (char)(0x80 | (c & 0x3F)); }
        else { r[k++] = (char)(0xF0 | (c >> 18)); r[k++] = (char)(0x80 | ((c >> 12) & 0x3F)); r[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); r[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    r[k] = 0; return r;
}
int jstr_equals(JObj *a, JObj *b) {
    if (a == b) return 1;
    if (!a || !b || a->cls != &rt_String_class || b->cls != &rt_String_class) return 0;
    JString *x = (JString *)a, *y = (JString *)b;
    return x->len == y->len && !memcmp(x->chars, y->chars, sizeof(uint16_t) * (size_t)x->len);
}
static int32_t str_hash(JString *s) {
    if (!s->hash) { uint32_t h = 0; for (int32_t i = 0; i < s->len; i++) h = 31u * h + s->chars[i]; s->hash = (int32_t)h; }
    return s->hash;
}

void JL(String_init__AB_I_I)(JObj *self, JObj *b, int32_t off, int32_t len) {
    JArray *a = (JArray *)NN(b);
    if (off < 0 || len < 0 || off + len > a->len) rt_throw(&rt_IndexOutOfBoundsException_class, NULL);
    JString *r = (JString *)jstr_decode(a->data + off, len, NULL); JString *s = S(self); s->len = r->len; s->chars = r->chars;
}
void JL(String_init__AB_I_I_Ljava_lang_String)(JObj *self, JObj *b, int32_t off, int32_t len, JObj *enc) {
    JArray *a = (JArray *)NN(b); char *e = jstr_dup_utf8(NN(enc));
    if (off < 0 || len < 0 || off + len > a->len) rt_throw(&rt_IndexOutOfBoundsException_class, NULL);
    JString *r = (JString *)jstr_decode(a->data + off, len, e); free(e); JString *s = S(self); s->len = r->len; s->chars = r->chars;
}
void JL(String_init__AB_Ljava_lang_String)(JObj *self, JObj *b, JObj *enc) {
    JArray *a = (JArray *)NN(b); char *e = jstr_dup_utf8(NN(enc));
    JString *r = (JString *)jstr_decode(a->data, a->len, e); free(e); JString *s = S(self); s->len = r->len; s->chars = r->chars;
}
void JL(String_init__AC_I_I)(JObj *self, JObj *c, int32_t off, int32_t len) {
    JArray *a = (JArray *)NN(c);
    if (off < 0 || len < 0 || off + len > a->len) rt_throw(&rt_StringIndexOutOfBoundsException_class, NULL);
    JString *r = (JString *)jstr_new((uint16_t *)a->data + off, len); JString *s = S(self); s->len = r->len; s->chars = r->chars;
}
int32_t JL(String_length__)(JObj *self) { return S(self)->len; }
int32_t JL(String_charAt__I)(JObj *self, int32_t i) {
    JString *s = S(self);
    if ((uint32_t)i >= (uint32_t)s->len) rt_throw(&rt_StringIndexOutOfBoundsException_class, NULL);
    return s->chars[i];
}
int32_t JL(String_compareTo__Ljava_lang_String)(JObj *self, JObj *o) {
    JString *a = S(self), *b = S(o); int32_t n = a->len < b->len ? a->len : b->len;
    for (int32_t i = 0; i < n; i++) if (a->chars[i] != b->chars[i]) return (int32_t)a->chars[i] - (int32_t)b->chars[i];
    return a->len - b->len;
}
int32_t JL(String_equals__Ljava_lang_Object)(JObj *self, JObj *o) { S(self); return jstr_equals(self, o); }
static int32_t index_of_ch(JString *s, int32_t ch, int32_t from) {
    if (from < 0) from = 0;
    for (int32_t i = from; i < s->len; i++) if (s->chars[i] == ch) return i;
    return -1;
}
static int32_t index_of_str(JString *s, JString *t, int32_t from) {
    if (from < 0) from = 0;
    if (t->len == 0) return from <= s->len ? from : s->len;
    for (int32_t i = from; i + t->len <= s->len; i++)
        if (!memcmp(s->chars + i, t->chars, sizeof(uint16_t) * (size_t)t->len)) return i;
    return -1;
}
int32_t JL(String_indexOf__I)(JObj *self, int32_t c) { return index_of_ch(S(self), c, 0); }
int32_t JL(String_indexOf__I_I)(JObj *self, int32_t c, int32_t f) { return index_of_ch(S(self), c, f); }
int32_t JL(String_indexOf__Ljava_lang_String)(JObj *self, JObj *t) { return index_of_str(S(self), S(t), 0); }
int32_t JL(String_indexOf__Ljava_lang_String_I)(JObj *self, JObj *t, int32_t f) { return index_of_str(S(self), S(t), f); }
int32_t JL(String_startsWith__Ljava_lang_String)(JObj *self, JObj *p) {
    JString *s = S(self), *t = S(p);
    return t->len <= s->len && !memcmp(s->chars, t->chars, sizeof(uint16_t) * (size_t)t->len);
}
JObj *JL(String_substring__I_I)(JObj *self, int32_t b, int32_t e) {
    JString *s = S(self);
    if (b < 0 || e > s->len || b > e) rt_throw(&rt_StringIndexOutOfBoundsException_class, NULL);
    if (b == 0 && e == s->len) return self;
    return jstr_new(s->chars + b, e - b);
}
JObj *JL(String_substring__I)(JObj *self, int32_t b) { return JL(String_substring__I_I)(self, b, S(self)->len); }
JObj *JL(String_toUpperCase__)(JObj *self) {
    JString *s = S(self); uint16_t *t = jvm_alloc(sizeof(uint16_t) * (size_t)(s->len + 1));
    for (int32_t i = 0; i < s->len; i++) {
        uint16_t c = s->chars[i];
        if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 32; else if (c == 0xFF) c = 0x178;
        t[i] = c;
    }
    JObj *r = jstr_new(t, s->len); free(t); return r;
}
JObj *JL(String_trim__)(JObj *self) {
    JString *s = S(self); int32_t b = 0, e = s->len;
    while (b < e && s->chars[b] <= ' ') b++;
    while (e > b && s->chars[e - 1] <= ' ') e--;
    return (b == 0 && e == s->len) ? self : jstr_new(s->chars + b, e - b);
}
JObj *rt_new_java_lang_String(void) { return rt_alloc_obj(&rt_String_class); }

int32_t jobj_hash(JObj *o) {
    if (!o) return 0;
    if (o->cls == &rt_String_class) return str_hash((JString *)o);
    if (o->cls == &rt_Integer_class) return ((JInteger *)o)->v;
    return (int32_t)((uintptr_t)o >> 4);
}
int jobj_equals(JObj *a, JObj *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->cls == &rt_String_class) return jstr_equals(a, b);
    if (a->cls == &rt_Integer_class) return b->cls == &rt_Integer_class && ((JInteger *)a)->v == ((JInteger *)b)->v;
    return 0;
}

/* ============================================================= Integer */
JObj *rt_new_java_lang_Integer(void) { return rt_alloc_obj(&rt_Integer_class); }
void JL(Integer_init__I)(JObj *self, int32_t v) { ((JInteger *)NN(self))->v = v; }
int32_t JL(Integer_byteValue__)(JObj *self) { return (int8_t)((JInteger *)NN(self))->v; }
JObj *jint_new(int32_t v) { JObj *o = rt_alloc_obj(&rt_Integer_class); ((JInteger *)o)->v = v; return o; }

/* ======================================================== StringBuffer */
typedef struct JSB { JObj o; uint16_t *buf; int32_t len, cap; } JSB;
JClass rt_StringBuffer_class = { .name = "java/lang/StringBuffer", .super = &rt_Object_class, .size = sizeof(JSB), .init_state = 2 };
JObj *rt_new_java_lang_StringBuffer(void) { return rt_alloc_obj(&rt_StringBuffer_class); }
static void sb_reserve(JSB *b, int32_t extra) {
    if (b->len + extra <= b->cap) return;
    int32_t nc = b->cap ? b->cap * 2 : 16; while (nc < b->len + extra) nc *= 2;
    b->buf = realloc(b->buf, sizeof(uint16_t) * (size_t)nc); if (!b->buf) abort(); b->cap = nc;
}
static JObj *sb_add(JObj *self, const uint16_t *s, int32_t n) {
    JSB *b = (JSB *)NN(self);
    if (n <= 0) return self;                       /* appending "" must not touch a NULL buffer */
    sb_reserve(b, n); memcpy(b->buf + b->len, s, sizeof(uint16_t) * (size_t)n); b->len += n; return self;
}
static JObj *sb_add_ascii(JObj *self, const char *s) {
    uint16_t t[64]; int n = 0; while (s[n] && n < 64) { t[n] = (uint8_t)s[n]; n++; } return sb_add(self, t, n);
}
void JL(StringBuffer_init__)(JObj *self) { (void)self; }
JObj *JL(StringBuffer_append__C)(JObj *self, int32_t c) { uint16_t ch = (uint16_t)c; return sb_add(self, &ch, 1); }
JObj *JL(StringBuffer_append__I)(JObj *self, int32_t v) { char t[24]; snprintf(t, sizeof t, "%d", v); return sb_add_ascii(self, t); }
JObj *JL(StringBuffer_append__Z)(JObj *self, int32_t v) { return sb_add_ascii(self, v ? "true" : "false"); }
JObj *JL(StringBuffer_append__Ljava_lang_String)(JObj *self, JObj *s) {
    if (!s) return sb_add_ascii(self, "null");
    return sb_add(self, S(s)->chars, S(s)->len);
}
JObj *jstr_value_of(JObj *o) {
    if (!o) return jstr_from_utf8("null");
    if (o->cls == &rt_String_class) return o;
    if (o->cls == &rt_Integer_class) { char t[24]; snprintf(t, sizeof t, "%d", ((JInteger *)o)->v); return jstr_from_utf8(t); }
    if (o->cls == &rt_StringBuffer_class) return JL(StringBuffer_toString__)(o);
    char t[96]; snprintf(t, sizeof t, "%s@%x", o->cls->name, (unsigned)jobj_hash(o)); return jstr_from_utf8(t);
}
JObj *JL(StringBuffer_append__Ljava_lang_Object)(JObj *self, JObj *o) { JObj *s = jstr_value_of(o); return sb_add(self, S(s)->chars, S(s)->len); }
JObj *JL(StringBuffer_toString__)(JObj *self) { JSB *b = (JSB *)NN(self); return jstr_new(b->buf, b->len); }

/* ================================================================ Math */
int32_t JL(Math_abs__I)(int32_t a) { return a < 0 ? (int32_t)(0u - (uint32_t)a) : a; }
int32_t JL(Math_max__I_I)(int32_t a, int32_t b) { return a > b ? a : b; }
int32_t JL(Math_min__I_I)(int32_t a, int32_t b) { return a < b ? a : b; }
int64_t JL(Math_max__J_J)(int64_t a, int64_t b) { return a > b ? a : b; }
int64_t JL(Math_min__J_J)(int64_t a, int64_t b) { return a < b ? a : b; }

/* ============================================================== System */
JClass rt_PrintStream_class = { .name = "java/io/PrintStream", .super = &rt_Object_class, .size = sizeof(JObj), .init_state = 2 };
static JObj ps_out = { &rt_PrintStream_class };
JObj *rt_java_lang_System_out = &ps_out;
void JIO(PrintStream_println__Ljava_lang_String)(JObj *self, JObj *s) {
    (void)self; char *u = s ? jstr_dup_utf8(s) : NULL; fprintf(stdout, "[System.out] %s\n", u ? u : "null"); free(u); fflush(stdout);
}
int64_t JL(System_currentTimeMillis__)(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void JL(System_gc__)(void) {}
static _Atomic int32_t sleep_compensation_ms;
void rt_sleep_set_compensation(int32_t ms) { atomic_store(&sleep_compensation_ms, ms); }
int32_t rt_sleep_get_compensation(void) { return atomic_load(&sleep_compensation_ms); }
void JL(System_arraycopy__Ljava_lang_Object_I_Ljava_lang_Object_I_I)(JObj *src, int32_t sp, JObj *dst, int32_t dp, int32_t n) {
    JArray *s = (JArray *)NN(src), *d = (JArray *)NN(dst);
    if (s->o.cls != &rt_array_class || d->o.cls != &rt_array_class || s->kind != d->kind) rt_throw(&rt_RuntimeException_class, "ArrayStoreException");
    if (sp < 0 || dp < 0 || n < 0 || (int64_t)sp + n > s->len || (int64_t)dp + n > d->len) jvm_throw_aioobe(sp < 0 ? sp : dp);
    static const int es[13] = { 0, 0, 0, 0, 1, 2, 4, 8, 1, 2, 4, 8, sizeof(void *) };
    memmove(d->data + (size_t)dp * es[d->kind], s->data + (size_t)sp * es[s->kind], (size_t)n * es[s->kind]);
}
void JL(Thread_sleep__J)(int64_t ms) {
    if (ms < 0) rt_throw(&rt_IllegalArgumentException_class, NULL);
    if (ms > 4) {
        ms -= atomic_load(&sleep_compensation_ms);
        if (ms < 1) ms = 1;
    }
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL);
}

/* =========================================================== Object etc. */
static JObj class_obj = { &rt_Class_class };
JObj *JL(Object_getClass__)(JObj *self) { NN(self); return &class_obj; }
void JL(Throwable_printStackTrace__)(JObj *self) {
    JThrowable *t = (JThrowable *)NN(self); char *m = t->msg ? jstr_dup_utf8(t->msg) : NULL;
    fprintf(stderr, "%s%s%s\n", self->cls->name, m ? ": " : "", m ? m : ""); free(m);
}

/* ============================================================== Random */
typedef struct JRandom { JObj o; int64_t seed; } JRandom;
JClass rt_Random_class = { .name = "java/util/Random", .super = &rt_Object_class, .size = sizeof(JRandom), .init_state = 2 };
JObj *rt_new_java_util_Random(void) { return rt_alloc_obj(&rt_Random_class); }
void JU(Random_setSeed__J)(JObj *self, int64_t s) { ((JRandom *)NN(self))->seed = (s ^ 0x5DEECE66DLL) & ((1LL << 48) - 1); }
void JU(Random_init__J)(JObj *self, int64_t s) { JU(Random_setSeed__J)(self, s); }
int32_t JU(Random_nextInt__)(JObj *self) {
    JRandom *r = (JRandom *)NN(self);
    r->seed = (int64_t)(((uint64_t)r->seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1));
    return (int32_t)(r->seed >> 16);
}
