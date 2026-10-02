/*
 * jvm.h - object model and helpers shared by the generated (recompiled) game
 * code and the hand-written J2ME runtime.
 *
 * Design notes
 *  - Every Java reference is a JObj*. Concrete layouts are generated structs
 *    whose first member is the superclass struct, so a pointer to any object
 *    is also a pointer to its JObj header.
 *  - Java exceptions are implemented with setjmp/longjmp. Only methods that
 *    contain handlers pay for a frame (see JvmTry).
 *  - Memory is never freed (no GC yet). See runtime/ for the allocator hook.
 */
#ifndef JVM_H
#define JVM_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#if defined(__GNUC__)
#define JVM_LIKELY(x)   __builtin_expect(!!(x), 1)
#define JVM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define JVM_NORETURN    __attribute__((noreturn))
#define JVM_TLS         __thread
#else
#define JVM_LIKELY(x)   (x)
#define JVM_UNLIKELY(x) (x)
#define JVM_NORETURN
#define JVM_TLS
#endif

typedef struct JClass JClass;
typedef struct JObj   JObj;

struct JObj { JClass *cls; };

typedef struct JMethodInfo {
    const char *name;
    const char *desc;
    void       *fn;          /* C function implementing the method */
} JMethodInfo;

#define JCLASS_INTERFACE 0x1
#define JCLASS_ABSTRACT  0x2

struct JClass {
    const char          *name;      /* internal name as found in the class file */
    JClass              *super;     /* NULL when the superclass is not an app class */
    const char          *ext_super; /* name of the runtime (J2ME) superclass, if any */
    JClass *const       *ifaces;
    int                  nifaces;
    const char *const   *ext_ifaces; /* runtime interfaces implemented (e.g. java/lang/Runnable) */
    int                  next_ifaces;
    uint32_t             size;      /* sizeof(struct) for allocation */
    uint32_t             flags;
    void *const         *vtable;    /* indexed by selector number; NULL if none */
    int                  nvt;
    const JMethodInfo   *methods;   /* own virtual methods that have code */
    int                  nmethods;
    void               (*clinit)(void);
    volatile int         init_state; /* 0 = not run, 1 = running, 2 = done */
};

/* ------------------------------------------------------------------ */
/* Exceptions                                                          */
/* ------------------------------------------------------------------ */
typedef struct JvmTry {
    struct JvmTry *prev;
    jmp_buf        jb;
} JvmTry;

extern JVM_TLS JvmTry *jvm_try_top;
extern JVM_TLS JObj   *jvm_exc;

JVM_NORETURN void jvm_throw(JObj *ex);
JVM_NORETURN void jvm_throw_npe(void);
JVM_NORETURN void jvm_throw_aioobe(int32_t index);
JVM_NORETURN void jvm_throw_arith(void);
JVM_NORETURN void jvm_throw_negsize(void);
JVM_NORETURN void jvm_throw_cce(void);
JVM_NORETURN void jvm_unreachable(const char *what);

/* ------------------------------------------------------------------ */
/* Arrays                                                              */
/* ------------------------------------------------------------------ */
enum { JA_BOOL = 4, JA_CHAR = 5, JA_FLOAT = 6, JA_DOUBLE = 7,
       JA_BYTE = 8, JA_SHORT = 9, JA_INT = 10, JA_LONG = 11, JA_REF = 12 };

typedef struct JArray {
    JObj        o;
    int32_t     len;
    int32_t     kind;      /* JA_* */
    const char *edesc;     /* element descriptor for JA_REF arrays, else NULL */
    _Alignas(8) uint8_t data[];
} JArray;

JObj *jvm_newarray(int32_t atype, int32_t n);
JObj *jvm_newarray_ref(int32_t n, const char *edesc);
JObj *jvm_multianewarray(const char *desc, int32_t ndims, const int32_t *dims);

#define JVM_ARRAY_ACCESSORS(SUF, T)                                          \
static inline T jaload_##SUF(JObj *o, int32_t i) {                           \
    JArray *a = (JArray *)o;                                                 \
    if (JVM_UNLIKELY(!a)) jvm_throw_npe();                                   \
    if (JVM_UNLIKELY((uint32_t)i >= (uint32_t)a->len)) jvm_throw_aioobe(i);  \
    return ((T *)a->data)[i];                                                \
}                                                                            \
static inline void jastore_##SUF(JObj *o, int32_t i, T v) {                  \
    JArray *a = (JArray *)o;                                                 \
    if (JVM_UNLIKELY(!a)) jvm_throw_npe();                                   \
    if (JVM_UNLIKELY((uint32_t)i >= (uint32_t)a->len)) jvm_throw_aioobe(i);  \
    ((T *)a->data)[i] = v;                                                   \
}
JVM_ARRAY_ACCESSORS(B, int8_t)
JVM_ARRAY_ACCESSORS(C, uint16_t)
JVM_ARRAY_ACCESSORS(S, int16_t)
JVM_ARRAY_ACCESSORS(I, int32_t)
JVM_ARRAY_ACCESSORS(J, int64_t)
JVM_ARRAY_ACCESSORS(F, float)
JVM_ARRAY_ACCESSORS(D, double)
JVM_ARRAY_ACCESSORS(A, JObj *)

static inline int32_t jvm_arraylength(JObj *o) {
    if (JVM_UNLIKELY(!o)) jvm_throw_npe();
    return ((JArray *)o)->len;
}

/* ------------------------------------------------------------------ */
/* Core operations used by generated code                              */
/* ------------------------------------------------------------------ */
static inline JObj *jvm_nn(JObj *o) {
    if (JVM_UNLIKELY(!o)) jvm_throw_npe();
    return o;
}

JObj *jvm_new(JClass *c);
void  jvm_class_init(JClass *c);
JObj *jvm_str_lit(int idx);

int   jvm_instanceof(JObj *o, const JClass *c);
int   jvm_instanceof_ext(JObj *o, const char *name);
int   jvm_instanceof_array(JObj *o, const char *desc);
JObj *jvm_checkcast(JObj *o, const JClass *c);
JObj *jvm_checkcast_ext(JObj *o, const char *name);
JObj *jvm_checkcast_array(JObj *o, const char *desc);

/* Virtual dispatch through the per-class selector table. */
static inline void *jvm_vlookup(JObj *o, int sel) {
    if (JVM_UNLIKELY(!o)) jvm_throw_npe();
    return o->cls->vtable[sel];
}
/* Name based lookup (used by the runtime to call back into the game). */
void *jvm_find_method(const JClass *c, const char *name, const char *desc);

/* Integer / floating point semantics */
static inline int32_t jvm_idiv(int32_t a, int32_t b) {
    if (JVM_UNLIKELY(b == 0)) jvm_throw_arith();
    if (JVM_UNLIKELY(b == -1)) return (int32_t)(0u - (uint32_t)a);
    return a / b;
}
static inline int32_t jvm_irem(int32_t a, int32_t b) {
    if (JVM_UNLIKELY(b == 0)) jvm_throw_arith();
    if (JVM_UNLIKELY(b == -1)) return 0;
    return a % b;
}
static inline int64_t jvm_ldiv(int64_t a, int64_t b) {
    if (JVM_UNLIKELY(b == 0)) jvm_throw_arith();
    if (JVM_UNLIKELY(b == -1)) return (int64_t)(0ull - (uint64_t)a);
    return a / b;
}
static inline int64_t jvm_lrem(int64_t a, int64_t b) {
    if (JVM_UNLIKELY(b == 0)) jvm_throw_arith();
    if (JVM_UNLIKELY(b == -1)) return 0;
    return a % b;
}
static inline int32_t jvm_d2i(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}
static inline int64_t jvm_d2l(double d) {
    if (d != d) return 0;
    if (d >= 9223372036854775807.0) return INT64_MAX;
    if (d <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)d;
}
static inline int32_t jvm_f2i(float f) { return jvm_d2i((double)f); }
static inline int64_t jvm_f2l(float f) { return jvm_d2l((double)f); }
static inline int32_t jvm_lcmp(int64_t a, int64_t b) { return (a > b) - (a < b); }
static inline int32_t jvm_fcmpl(double a, double b) { return (a != a || b != b) ? -1 : (a > b) - (a < b); }
static inline int32_t jvm_fcmpg(double a, double b) { return (a != a || b != b) ?  1 : (a > b) - (a < b); }

/* ------------------------------------------------------------------ */
/* Layouts of runtime (J2ME) classes that game classes extend          */
/* ------------------------------------------------------------------ */
typedef struct rt_Canvas {
    JObj  o;
    void *priv;              /* owned by runtime/canvas.c */
} rt_Canvas;

typedef struct rt_MIDlet {
    JObj  o;
    void *priv;
} rt_MIDlet;

/* String literal table emitted into gen/jstrings.c */
typedef struct JStrLit { int32_t len; const uint16_t *chars; } JStrLit;
extern const JStrLit jvm_str_table[];
extern const int     jvm_str_count;

#endif /* JVM_H */
