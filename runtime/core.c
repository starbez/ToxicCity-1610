/* core.c - object model support: allocation, class init, casts, arrays, exceptions */
#include "rt.h"
#include <stdarg.h>

JVM_TLS JvmTry *jvm_try_top;
JVM_TLS JObj   *jvm_exc;

/* ---------------------------------------------------------------- classes */
#define RTC(var, nm, sup, sz) JClass var = { .name = nm, .super = sup, .size = sz, .init_state = 2 }
RTC(rt_Object_class, "java/lang/Object", NULL, sizeof(JObj));
RTC(rt_Class_class, "java/lang/Class", &rt_Object_class, sizeof(JObj));
RTC(rt_String_class, "java/lang/String", &rt_Object_class, sizeof(JString));
RTC(rt_Integer_class, "java/lang/Integer", &rt_Object_class, sizeof(JInteger));
RTC(rt_array_class, "[array", &rt_Object_class, sizeof(JArray));

RTC(rt_Throwable_class, "java/lang/Throwable", &rt_Object_class, sizeof(JThrowable));
RTC(rt_Exception_class, "java/lang/Exception", &rt_Throwable_class, sizeof(JThrowable));
RTC(rt_Error_class, "java/lang/Error", &rt_Throwable_class, sizeof(JThrowable));
RTC(rt_OutOfMemoryError_class, "java/lang/OutOfMemoryError", &rt_Error_class, sizeof(JThrowable));
RTC(rt_RuntimeException_class, "java/lang/RuntimeException", &rt_Exception_class, sizeof(JThrowable));
RTC(rt_NullPointerException_class, "java/lang/NullPointerException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_ArithmeticException_class, "java/lang/ArithmeticException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_ClassCastException_class, "java/lang/ClassCastException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_NegativeArraySizeException_class, "java/lang/NegativeArraySizeException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_IllegalArgumentException_class, "java/lang/IllegalArgumentException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_IllegalStateException_class, "java/lang/IllegalStateException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_IndexOutOfBoundsException_class, "java/lang/IndexOutOfBoundsException", &rt_RuntimeException_class, sizeof(JThrowable));
RTC(rt_ArrayIndexOutOfBoundsException_class, "java/lang/ArrayIndexOutOfBoundsException", &rt_IndexOutOfBoundsException_class, sizeof(JThrowable));
RTC(rt_StringIndexOutOfBoundsException_class, "java/lang/StringIndexOutOfBoundsException", &rt_IndexOutOfBoundsException_class, sizeof(JThrowable));
RTC(rt_IOException_class, "java/io/IOException", &rt_Exception_class, sizeof(JThrowable));
RTC(rt_RecordStoreException_class, "javax/microedition/rms/RecordStoreException", &rt_Exception_class, sizeof(JThrowable));
RTC(rt_RecordStoreNotFoundException_class, "javax/microedition/rms/RecordStoreNotFoundException", &rt_RecordStoreException_class, sizeof(JThrowable));
RTC(rt_InvalidRecordIDException_class, "javax/microedition/rms/InvalidRecordIDException", &rt_RecordStoreException_class, sizeof(JThrowable));
RTC(rt_RecordStoreFullException_class, "javax/microedition/rms/RecordStoreFullException", &rt_RecordStoreException_class, sizeof(JThrowable));
RTC(rt_MediaException_class, "javax/microedition/media/MediaException", &rt_Exception_class, sizeof(JThrowable));

JClass *rt_class_by_name(const char *name) {
    static JClass *const tab[] = {
        &rt_Object_class, &rt_Class_class, &rt_String_class, &rt_StringBuffer_class, &rt_Integer_class,
        &rt_Thread_class, &rt_Hashtable_class, &rt_Random_class, &rt_PrintStream_class,
        &rt_InputStream_class, &rt_ByteArrayInputStream_class, &rt_array_class,
        &rt_Throwable_class, &rt_Exception_class, &rt_Error_class, &rt_OutOfMemoryError_class,
        &rt_RuntimeException_class, &rt_NullPointerException_class, &rt_ArithmeticException_class,
        &rt_ClassCastException_class, &rt_NegativeArraySizeException_class,
        &rt_IllegalArgumentException_class, &rt_IllegalStateException_class,
        &rt_IndexOutOfBoundsException_class, &rt_ArrayIndexOutOfBoundsException_class,
        &rt_StringIndexOutOfBoundsException_class, &rt_IOException_class,
        &rt_RecordStoreException_class, &rt_RecordStoreNotFoundException_class,
        &rt_InvalidRecordIDException_class, &rt_RecordStoreFullException_class,
        &rt_MediaException_class, &rt_MIDlet_class, &rt_Canvas_class, &rt_Displayable_class,
        &rt_Display_class, &rt_Command_class, &rt_Graphics_class, &rt_Image_class, &rt_Font_class,
        &rt_RecordStore_class, &rt_Player_class, &rt_VolumeControl_class,
    };
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
        if (!strcmp(tab[i]->name, name)) return tab[i];
    return NULL;
}

/* ------------------------------------------------------------- allocation */
void *jvm_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "jvm: out of memory (%zu bytes)\n", n); abort(); }
    return p;
}
JObj *rt_alloc_obj(JClass *c) { JObj *o = jvm_alloc(c->size); o->cls = c; return o; }
JObj *jvm_new(JClass *c) {
    if (c->flags & (JCLASS_ABSTRACT | JCLASS_INTERFACE)) jvm_unreachable("instantiating abstract class");
    return rt_alloc_obj(c);
}

/* ---------------------------------------------------------- class init */
static pthread_mutex_t init_mu;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static void init_mutex(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&init_mu, &a);
}
void jvm_class_init(JClass *c) {
    if (c->init_state == 2) return;
    pthread_once(&init_once, init_mutex);
    pthread_mutex_lock(&init_mu);
    if (c->init_state == 0) {
        c->init_state = 1;
        if (c->super) jvm_class_init(c->super);
        if (c->clinit) c->clinit();
        c->init_state = 2;
    }
    pthread_mutex_unlock(&init_mu);
}

/* ---------------------------------------------------------- type checks */
static int cls_is(const JClass *k, const JClass *t) {
    for (; k; k = k->super) {
        if (k == t) return 1;
        for (int i = 0; i < k->nifaces; i++) if (cls_is(k->ifaces[i], t)) return 1;
    }
    return 0;
}
int jvm_instanceof(JObj *o, const JClass *c) { return o && cls_is(o->cls, c); }

static int ext_is(const JClass *k, const char *name) {
    while (k) {
        if (!strcmp(k->name, name)) return 1;
        for (int i = 0; i < k->next_ifaces; i++) if (!strcmp(k->ext_ifaces[i], name)) return 1;
        for (int i = 0; i < k->nifaces; i++) if (ext_is(k->ifaces[i], name)) return 1;
        if (k->super) k = k->super;
        else if (k->ext_super) {
            if (!strcmp(k->ext_super, name)) return 1;
            k = rt_class_by_name(k->ext_super);
        } else k = NULL;
    }
    return 0;
}
int jvm_instanceof_ext(JObj *o, const char *name) {
    if (!o) return 0;
    if (!strcmp(name, "java/lang/Object")) return 1;
    return ext_is(o->cls, name);
}
int jvm_instanceof_array(JObj *o, const char *desc) {
    if (!o || o->cls != &rt_array_class) return 0;
    JArray *a = (JArray *)o;
    switch (desc[1]) {
    case 'Z': return a->kind == JA_BOOL || a->kind == JA_BYTE;
    case 'B': return a->kind == JA_BYTE || a->kind == JA_BOOL;
    case 'C': return a->kind == JA_CHAR;
    case 'S': return a->kind == JA_SHORT;
    case 'I': return a->kind == JA_INT;
    case 'J': return a->kind == JA_LONG;
    case 'F': return a->kind == JA_FLOAT;
    case 'D': return a->kind == JA_DOUBLE;
    default:
        if (a->kind != JA_REF) return 0;
        if (!strcmp(desc, "[Ljava/lang/Object;")) return 1;
        return !a->edesc || !strcmp(a->edesc, desc + 1) || 1; /* element type not enforced yet */
    }
}
JObj *jvm_checkcast(JObj *o, const JClass *c) { if (o && !cls_is(o->cls, c)) jvm_throw_cce(); return o; }
JObj *jvm_checkcast_ext(JObj *o, const char *n) { if (o && !jvm_instanceof_ext(o, n)) jvm_throw_cce(); return o; }
JObj *jvm_checkcast_array(JObj *o, const char *d) { if (o && !jvm_instanceof_array(o, d)) jvm_throw_cce(); return o; }

void *jvm_find_method(const JClass *c, const char *name, const char *desc) {
    for (; c; c = c->super)
        for (int i = 0; i < c->nmethods; i++)
            if (!strcmp(c->methods[i].name, name) && !strcmp(c->methods[i].desc, desc)) return c->methods[i].fn;
    return NULL;
}

/* --------------------------------------------------------------- arrays */
static const int esize[13] = { 0, 0, 0, 0, 1, 2, 4, 8, 1, 2, 4, 8, sizeof(void *) };
static JObj *new_array(int kind, int32_t n, const char *edesc) {
    if (n < 0) jvm_throw_negsize();
    JArray *a = jvm_alloc(sizeof(JArray) + (size_t)n * esize[kind]);
    a->o.cls = &rt_array_class; a->len = n; a->kind = kind; a->edesc = edesc;
    return &a->o;
}
JObj *jvm_newarray(int32_t atype, int32_t n) { return new_array(atype, n, NULL); }
JObj *jvm_newarray_ref(int32_t n, const char *edesc) { return new_array(JA_REF, n, edesc); }

static int atype_of(char c) {
    switch (c) { case 'Z': return JA_BOOL; case 'C': return JA_CHAR; case 'F': return JA_FLOAT;
    case 'D': return JA_DOUBLE; case 'B': return JA_BYTE; case 'S': return JA_SHORT;
    case 'I': return JA_INT; case 'J': return JA_LONG; default: return JA_REF; }
}
JObj *jvm_multianewarray(const char *desc, int32_t ndims, const int32_t *dims) {
    const char *elem = desc + 1;                 /* descriptor of the element type */
    for (int i = 0; i < ndims; i++) if (dims[i] < 0) jvm_throw_negsize();
    if (ndims == 1) {
        int k = atype_of(elem[0]);
        return new_array(k, dims[0], k == JA_REF ? elem : NULL);
    }
    JObj *top = new_array(JA_REF, dims[0], elem);
    for (int32_t i = 0; i < dims[0]; i++)
        ((JObj **)((JArray *)top)->data)[i] = jvm_multianewarray(elem, ndims - 1, dims + 1);
    return top;
}

/* ------------------------------------------------------------ exceptions */
JObj *rt_new_exception(JClass *c, const char *msg) {
    JThrowable *t = (JThrowable *)rt_alloc_obj(c);
    if (msg) t->msg = jstr_from_utf8(msg);
    return &t->o;
}
static int trace_exc = -1;
void jvm_throw(JObj *ex) {
    jvm_exc = ex;
    if (trace_exc < 0) trace_exc = getenv("JVM_TRACE") != NULL;
    if (trace_exc && ex) {
        JThrowable *th = (JThrowable *)ex; char *m = jstr_dup_utf8(th->msg);
        fprintf(stderr, "[throw] %s%s%s\n", ex->cls->name, m ? ": " : "", m ? m : ""); free(m);
    }
    JvmTry *t = jvm_try_top;
    if (!t) {
        JThrowable *th = (JThrowable *)ex;
        fprintf(stderr, "jvm: uncaught exception %s", ex ? ex->cls->name : "(null)");
        if (ex && jvm_instanceof_ext(ex, "java/lang/Throwable") && th->msg) {
            char *s = jstr_dup_utf8(th->msg); fprintf(stderr, ": %s", s); free(s);
        }
        fputc('\n', stderr); fflush(stderr); abort();
    }
    jvm_try_top = t->prev;
    longjmp(t->jb, 1);
}
void rt_throw(JClass *c, const char *msg) { jvm_throw(rt_new_exception(c, msg)); }
void jvm_throw_npe(void)  { rt_throw(&rt_NullPointerException_class, NULL); }
void jvm_throw_arith(void){ rt_throw(&rt_ArithmeticException_class, "/ by zero"); }
void jvm_throw_negsize(void) { rt_throw(&rt_NegativeArraySizeException_class, NULL); }
void jvm_throw_cce(void)  { rt_throw(&rt_ClassCastException_class, NULL); }
void jvm_throw_aioobe(int32_t i) {
    char b[48]; snprintf(b, sizeof b, "%d", i);
    rt_throw(&rt_ArrayIndexOutOfBoundsException_class, b);
}
void jvm_unreachable(const char *what) {
    fprintf(stderr, "jvm: fatal: %s\n", what); fflush(stderr); abort();
}

/* ------------------------------------------------- string literal table */
JObj *jvm_str_lit(int idx) {
    static JObj **cache; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (idx < 0 || idx >= jvm_str_count) jvm_unreachable("bad string literal index");
    pthread_mutex_lock(&mu);
    if (!cache) cache = jvm_alloc(sizeof(JObj *) * (size_t)jvm_str_count);
    JObj *s = cache[idx];
    if (!s) s = cache[idx] = jstr_new(jvm_str_table[idx].chars, jvm_str_table[idx].len);
    pthread_mutex_unlock(&mu);
    return s;
}

/* -------------------------------------------------------- diagnostics */
#define MAXC 512
static struct { const char *k; long n; } counts[MAXC]; static int ncounts;
static pthread_mutex_t cmu = PTHREAD_MUTEX_INITIALIZER;
void rt_count(const char *api) {
    pthread_mutex_lock(&cmu);
    for (int i = 0; i < ncounts; i++) if (counts[i].k == api || !strcmp(counts[i].k, api)) { counts[i].n++; goto out; }
    if (ncounts < MAXC) { counts[ncounts].k = api; counts[ncounts++].n = 1; }
out: pthread_mutex_unlock(&cmu);
}
long rt_count_value(const char *api) {
    long v = 0; pthread_mutex_lock(&cmu);
    for (int i = 0; i < ncounts; i++) if (!strcmp(counts[i].k, api)) v = counts[i].n;
    pthread_mutex_unlock(&cmu); return v;
}
void rt_dump_counts(FILE *f) {
    pthread_mutex_lock(&cmu);
    for (int i = 0; i < ncounts; i++) fprintf(f, "  %-40s %ld\n", counts[i].k, counts[i].n);
    pthread_mutex_unlock(&cmu);
}
