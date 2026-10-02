/* io.c - InputStream / ByteArrayInputStream, JAR resources, manifest properties */
#include "rt.h"
#include <errno.h>

const char *rt_res_dir = "res";
const char *rt_save_dir = "save";
volatile int rt_quit;

typedef struct JStream { JObj o; JObj *backing; const uint8_t *data; int32_t len, pos; } JStream;
JClass rt_InputStream_class = { .name = "java/io/InputStream", .super = &rt_Object_class, .size = sizeof(JStream), .init_state = 2 };
JClass rt_ByteArrayInputStream_class = { .name = "java/io/ByteArrayInputStream", .super = &rt_InputStream_class, .size = sizeof(JStream), .init_state = 2 };

JObj *rt_new_java_io_ByteArrayInputStream(void) { return rt_alloc_obj(&rt_ByteArrayInputStream_class); }
void JIO(ByteArrayInputStream_init__AB)(JObj *self, JObj *arr) {
    JStream *s = (JStream *)NN(self); JArray *a = (JArray *)NN(arr);
    s->backing = arr; s->data = a->data; s->len = a->len;
}
JObj *rt_stream_from_memory(const uint8_t *data, int32_t len) {
    JStream *s = (JStream *)rt_alloc_obj(&rt_InputStream_class); s->data = data; s->len = len; return &s->o;
}
static JStream *ST(JObj *o) { return (JStream *)NN(o); }
int32_t JIO(InputStream_read__)(JObj *self) { JStream *s = ST(self); return s->pos < s->len ? s->data[s->pos++] : -1; }
int32_t JIO(InputStream_read__AB_I_I)(JObj *self, JObj *b, int32_t off, int32_t n) {
    JStream *s = ST(self); JArray *a = (JArray *)NN(b);
    if (off < 0 || n < 0 || off + n > a->len) rt_throw(&rt_IndexOutOfBoundsException_class, NULL);
    if (n == 0) return 0;
    if (s->pos >= s->len) return -1;
    if (n > s->len - s->pos) n = s->len - s->pos;
    memcpy(a->data + off, s->data + s->pos, (size_t)n); s->pos += n; return n;
}
int32_t JIO(InputStream_read__AB)(JObj *self, JObj *b) { return JIO(InputStream_read__AB_I_I)(self, b, 0, ((JArray *)NN(b))->len); }
int64_t JIO(InputStream_skip__J)(JObj *self, int64_t n) {
    JStream *s = ST(self); if (n <= 0) return 0;
    int64_t k = s->len - s->pos; if (n < k) k = n; s->pos += (int32_t)k; return k;
}
void JIO(InputStream_close__)(JObj *self) { ST(self); }

uint8_t *rt_read_file(const char *path, int32_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = jvm_alloc((size_t)n + 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); *len = (int32_t)n; return b;
}
JObj *JL(Class_getResourceAsStream__Ljava_lang_String)(JObj *self, JObj *name) {
    (void)self; char *n = jstr_dup_utf8(NN(name));
    const char *rel = n[0] == '/' ? n + 1 : n;
    if (strstr(rel, "..")) { free(n); return NULL; }              /* stay inside the resource dir */
    char path[1024]; snprintf(path, sizeof path, "%s/%s", rt_res_dir, rel);
    int32_t len; uint8_t *data = rt_read_file(path, &len); free(n);
    return data ? rt_stream_from_memory(data, len) : NULL;
}

/* ---- manifest / JAD properties ---- */
static char *mf_keys[128], *mf_vals[128]; static int mf_n;
static pthread_once_t mf_once = PTHREAD_ONCE_INIT;
static void mf_load(void) {
    char path[1024]; snprintf(path, sizeof path, "%s/META-INF/MANIFEST.MF", rt_res_dir);
    FILE *f = fopen(path, "r"); if (!f) return; char line[1024];
    while (fgets(line, sizeof line, f) && mf_n < 128) {
        char *c = strstr(line, ": "); if (!c) continue; *c = 0; char *v = c + 2; v[strcspn(v, "\r\n")] = 0;
        mf_keys[mf_n] = strdup(line); mf_vals[mf_n] = strdup(v); mf_n++;
    }
    fclose(f);
}
const char *rt_manifest_prop(const char *key) {
    pthread_once(&mf_once, mf_load);
    for (int i = 0; i < mf_n; i++) if (!strcmp(mf_keys[i], key)) return mf_vals[i];
    return NULL;
}
