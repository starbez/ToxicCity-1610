/* midlet.c - javax.microedition.midlet.MIDlet */
#include "rt.h"

JClass rt_MIDlet_class = { .name = "javax/microedition/midlet/MIDlet", .super = &rt_Object_class, .size = sizeof(rt_MIDlet), .init_state = 2 };
void MIDLET(MIDlet_init__)(JObj *self) { (void)self; }
JObj *MIDLET(MIDlet_getAppProperty__Ljava_lang_String)(JObj *self, JObj *key) {
    (void)self; char *k = jstr_dup_utf8(NN(key)); const char *v = rt_manifest_prop(k);
    rt_count("MIDlet.getAppProperty"); free(k); return v ? jstr_from_utf8(v) : NULL;
}
void MIDLET(MIDlet_notifyDestroyed__)(JObj *self) { (void)self; rt_quit = 1; if (getenv("JVM_TRACE")) fprintf(stderr, "[midlet] notifyDestroyed\n"); }
int32_t MIDLET(MIDlet_platformRequest__Ljava_lang_String)(JObj *self, JObj *url) {
    (void)self; char *u = jstr_dup_utf8(NN(url)); fprintf(stderr, "[platformRequest] %s (ignored)\n", u); free(u); return 0;
}
