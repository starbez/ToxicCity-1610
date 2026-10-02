/* util.c - java.util.Hashtable (chained buckets, Java equals/hashCode semantics for String/Integer) */
#include "rt.h"

typedef struct HEnt { JObj *k, *v; int32_t h; struct HEnt *next; } HEnt;
typedef struct JHashtable { JObj o; HEnt **b; int32_t nb, n; pthread_mutex_t mu; } JHashtable;
JClass rt_Hashtable_class = { .name = "java/util/Hashtable", .super = &rt_Object_class, .size = sizeof(JHashtable), .init_state = 2 };
JObj *rt_new_java_util_Hashtable(void) { return rt_alloc_obj(&rt_Hashtable_class); }
void JU(Hashtable_init__)(JObj *self) {
    JHashtable *t = (JHashtable *)NN(self); t->nb = 11; t->b = jvm_alloc(sizeof(HEnt *) * 11);
    pthread_mutex_init(&t->mu, NULL);
}
static void rehash(JHashtable *t) {
    int32_t nn = t->nb * 2 + 1; HEnt **nb = jvm_alloc(sizeof(HEnt *) * (size_t)nn);
    for (int32_t i = 0; i < t->nb; i++) for (HEnt *e = t->b[i], *nx; e; e = nx) { nx = e->next; uint32_t j = (uint32_t)e->h % (uint32_t)nn; e->next = nb[j]; nb[j] = e; }
    free(t->b); t->b = nb; t->nb = nn;
}
JObj *JU(Hashtable_put__Ljava_lang_Object_Ljava_lang_Object)(JObj *self, JObj *k, JObj *v) {
    JHashtable *t = (JHashtable *)NN(self); NN(k); NN(v);
    int32_t h = jobj_hash(k) & 0x7FFFFFFF; JObj *old = NULL;
    pthread_mutex_lock(&t->mu);
    for (HEnt *e = t->b[h % t->nb]; e; e = e->next) if (e->h == h && jobj_equals(e->k, k)) { old = e->v; e->v = v; pthread_mutex_unlock(&t->mu); return old; }
    if (t->n >= t->nb * 3 / 4) rehash(t);
    HEnt *e = jvm_alloc(sizeof *e); e->k = k; e->v = v; e->h = h; uint32_t j = (uint32_t)h % (uint32_t)t->nb; e->next = t->b[j]; t->b[j] = e; t->n++;
    pthread_mutex_unlock(&t->mu);
    return NULL;
}
JObj *JU(Hashtable_get__Ljava_lang_Object)(JObj *self, JObj *k) {
    JHashtable *t = (JHashtable *)NN(self); NN(k); int32_t h = jobj_hash(k) & 0x7FFFFFFF; JObj *r = NULL;
    pthread_mutex_lock(&t->mu);
    for (HEnt *e = t->b[h % t->nb]; e; e = e->next) if (e->h == h && jobj_equals(e->k, k)) { r = e->v; break; }
    pthread_mutex_unlock(&t->mu);
    return r;
}
JObj *JU(Hashtable_remove__Ljava_lang_Object)(JObj *self, JObj *k) {
    JHashtable *t = (JHashtable *)NN(self); NN(k); int32_t h = jobj_hash(k) & 0x7FFFFFFF; JObj *r = NULL;
    pthread_mutex_lock(&t->mu);
    for (HEnt **pp = &t->b[h % t->nb]; *pp; pp = &(*pp)->next) if ((*pp)->h == h && jobj_equals((*pp)->k, k)) { HEnt *e = *pp; r = e->v; *pp = e->next; t->n--; break; }
    pthread_mutex_unlock(&t->mu);
    return r;
}
