/* rms.c - javax.microedition.rms.RecordStore, persisted as one file per store under rt_save_dir */
#include "rt.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define rt_mkdir(path, mode) _mkdir(path)
#else
#define rt_mkdir(path, mode) mkdir(path, mode)
#endif

typedef struct Rec { uint8_t *d; int32_t n; } Rec;
typedef struct JRecordStore { JObj o; char *name; Rec *r; int32_t nr, cap; pthread_mutex_t mu; } JRecordStore;
JClass rt_RecordStore_class = { .name = "javax/microedition/rms/RecordStore", .super = &rt_Object_class, .size = sizeof(JRecordStore), .init_state = 2 };

static void path_of(const char *name, char *out, size_t cap) {
    char safe[200]; size_t k = 0;
    for (; name[k] && k < sizeof safe - 1; k++) safe[k] = (name[k] == '/' || name[k] == '\\' || name[k] == '.') ? '_' : name[k];
    safe[k] = 0; snprintf(out, cap, "%s/%s.rms", rt_save_dir, safe);
}
static void store_save(JRecordStore *s) {
    char p[1024]; path_of(s->name, p, sizeof p); rt_mkdir(rt_save_dir, 0755);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fwrite("RMS1", 1, 4, f); fwrite(&s->nr, 4, 1, f);
    for (int32_t i = 0; i < s->nr; i++) { fwrite(&s->r[i].n, 4, 1, f); fwrite(s->r[i].d, 1, (size_t)s->r[i].n, f); }
    fclose(f);
}
static int store_load(JRecordStore *s) {
    char p[1024]; path_of(s->name, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    char m[4]; int32_t n = 0;
    if (fread(m, 1, 4, f) != 4 || memcmp(m, "RMS1", 4) || fread(&n, 4, 1, f) != 1 || n < 0 || n > (1 << 20)) { fclose(f); return 1; }
    s->r = jvm_alloc(sizeof(Rec) * (size_t)(n ? n : 1)); s->cap = n ? n : 1;
    for (int32_t i = 0; i < n; i++) {
        int32_t len; if (fread(&len, 4, 1, f) != 1 || len < 0) break;
        s->r[i].d = jvm_alloc((size_t)len); s->r[i].n = len;
        if (len && fread(s->r[i].d, 1, (size_t)len, f) != (size_t)len) break;
        s->nr = i + 1;
    }
    fclose(f); return 1;
}
JObj *RMS(RecordStore_openRecordStore__Ljava_lang_String_Z)(JObj *name, int32_t create) {
    JRecordStore *s = (JRecordStore *)rt_alloc_obj(&rt_RecordStore_class);
    s->name = jstr_dup_utf8(NN(name)); pthread_mutex_init(&s->mu, NULL);
    if (!store_load(s)) {
        if (!create) rt_throw(&rt_RecordStoreNotFoundException_class, s->name);
        s->r = jvm_alloc(sizeof(Rec) * 16); s->cap = 16; store_save(s);
    }
    return &s->o;
}
void RMS(RecordStore_closeRecordStore__)(JObj *self) { JRecordStore *s = (JRecordStore *)NN(self); pthread_mutex_lock(&s->mu); store_save(s); pthread_mutex_unlock(&s->mu); }
int32_t RMS(RecordStore_getNumRecords__)(JObj *self) { return ((JRecordStore *)NN(self))->nr; }
int32_t RMS(RecordStore_addRecord__AB_I_I)(JObj *self, JObj *data, int32_t off, int32_t len) {
    JRecordStore *s = (JRecordStore *)NN(self); JArray *a = data ? (JArray *)data : NULL;
    if (len < 0 || (len > 0 && (!a || off < 0 || off + len > a->len))) rt_throw(&rt_ArrayIndexOutOfBoundsException_class, NULL);
    pthread_mutex_lock(&s->mu);
    if (s->nr == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->r = realloc(s->r, sizeof(Rec) * (size_t)s->cap); }
    Rec *r = &s->r[s->nr]; r->d = jvm_alloc((size_t)len); r->n = len; if (len) memcpy(r->d, a->data + off, (size_t)len);
    int32_t id = ++s->nr; store_save(s); pthread_mutex_unlock(&s->mu); return id;
}
JObj *RMS(RecordStore_getRecord__I)(JObj *self, int32_t id) {
    JRecordStore *s = (JRecordStore *)NN(self);
    if (id < 1 || id > s->nr) rt_throw(&rt_InvalidRecordIDException_class, NULL);
    JObj *o = jvm_newarray(JA_BYTE, s->r[id - 1].n); memcpy(((JArray *)o)->data, s->r[id - 1].d, (size_t)s->r[id - 1].n); return o;
}
void RMS(RecordStore_setRecord__I_AB_I_I)(JObj *self, int32_t id, JObj *data, int32_t off, int32_t len) {
    JRecordStore *s = (JRecordStore *)NN(self); JArray *a = (JArray *)data;
    if (id < 1 || id > s->nr) rt_throw(&rt_InvalidRecordIDException_class, NULL);
    if (len < 0 || (len > 0 && (!a || off < 0 || off + len > a->len))) rt_throw(&rt_ArrayIndexOutOfBoundsException_class, NULL);
    pthread_mutex_lock(&s->mu);
    free(s->r[id - 1].d); s->r[id - 1].d = jvm_alloc((size_t)len); s->r[id - 1].n = len; if (len) memcpy(s->r[id - 1].d, a->data + off, (size_t)len);
    store_save(s); pthread_mutex_unlock(&s->mu);
}
