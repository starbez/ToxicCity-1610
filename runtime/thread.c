/* thread.c - java.lang.Thread on top of pthreads */
#include "rt.h"

typedef struct JThread { JObj o; JObj *target; pthread_t th; volatile int started, alive; } JThread;
JClass rt_Thread_class = { .name = "java/lang/Thread", .super = &rt_Object_class, .size = sizeof(JThread), .init_state = 2 };
static const char *const thread_ifaces[] = { "java/lang/Runnable" };
static void __attribute__((constructor)) thread_ctor(void) { rt_Thread_class.ext_ifaces = thread_ifaces; rt_Thread_class.next_ifaces = 1; }

JObj *rt_new_java_lang_Thread(void) { return rt_alloc_obj(&rt_Thread_class); }
void JL(Thread_init__Ljava_lang_Runnable)(JObj *self, JObj *target) { ((JThread *)NN(self))->target = target; }

static void *thread_main(void *p) {
    JThread *t = p; JvmTry base; base.prev = NULL; jvm_try_top = &base;
    if (!setjmp(base.jb)) {
        void (*run)(JObj *) = t->target ? (void (*)(JObj *))jvm_find_method(t->target->cls, "run", "()V") : NULL;
        if (run) run(t->target);
    } else {
        JThrowable *ex = (JThrowable *)jvm_exc;
        char *m = ex && ex->msg ? jstr_dup_utf8(ex->msg) : NULL;
        fprintf(stderr, "[thread] uncaught %s%s%s\n", ex ? ex->o.cls->name : "?", m ? ": " : "", m ? m : ""); free(m);
    }
    t->alive = 0;
    return NULL;
}
void JL(Thread_start__)(JObj *self) {
    JThread *t = (JThread *)NN(self);
    if (t->started) rt_throw(&rt_IllegalStateException_class, "thread already started");
    t->started = 1; t->alive = 1;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
    if (pthread_create(&t->th, &at, thread_main, t)) { t->alive = 0; rt_throw(&rt_OutOfMemoryError_class, "cannot start thread"); }
    pthread_detach(t->th);
}
int32_t JL(Thread_isAlive__)(JObj *self) { return ((JThread *)NN(self))->alive; }
