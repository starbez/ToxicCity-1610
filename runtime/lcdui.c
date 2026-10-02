/* lcdui.c - Display / Displayable / Canvas / Command / Font, the paint pump, and key delivery.
 * Image and Graphics live in gfx.c. */
#include "rt.h"
#include "gfx.h"
#include <unistd.h>

typedef struct JDisplayable { JObj o; } JDisplayable;
typedef struct JDisplay { JObj o; JObj *current; } JDisplay;
typedef struct JCommand { JObj o; JObj *label; int32_t type, prio; } JCommand;
typedef struct JFont { JObj o; int32_t face, style, size; } JFont;

JClass rt_Displayable_class = { .name = "javax/microedition/lcdui/Displayable", .super = &rt_Object_class, .size = sizeof(JDisplayable), .init_state = 2 };
JClass rt_Canvas_class = { .name = "javax/microedition/lcdui/Canvas", .super = &rt_Displayable_class, .size = sizeof(rt_Canvas), .init_state = 2 };
JClass rt_Display_class = { .name = "javax/microedition/lcdui/Display", .super = &rt_Object_class, .size = sizeof(JDisplay), .init_state = 2 };
JClass rt_Command_class = { .name = "javax/microedition/lcdui/Command", .super = &rt_Object_class, .size = sizeof(JCommand), .init_state = 2 };
JClass rt_Font_class = { .name = "javax/microedition/lcdui/Font", .super = &rt_Object_class, .size = sizeof(JFont), .init_state = 2 };

#define CNT(name) rt_count("lcdui." name)

/* ------------------------------------------------------------ Display */
static JDisplay display = { { &rt_Display_class }, NULL };
static pthread_mutex_t paint_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int repaint_pending;
static JImage *screen; static JGraphics *screen_g; static volatile uint64_t frame_counter;

JObj *rt_display_current(void) { return display.current; }
JObj *LCDUI(Display_getDisplay__Ljavax_microedition_midlet_MIDlet)(JObj *m) { NN(m); CNT("Display.getDisplay"); return &display.o; }
JObj *LCDUI(Display_getCurrent__)(JObj *self) { (void)self; return display.current; }
void LCDUI(Display_setCurrent__Ljavax_microedition_lcdui_Displayable)(JObj *self, JObj *d) {
    (void)self; CNT("Display.setCurrent"); display.current = d;
    if (!d) return;
    void (*sc)(JObj *, int32_t, int32_t) = jvm_find_method(d->cls, "sizeChanged", "(II)V");
    void (*sn)(JObj *) = jvm_find_method(d->cls, "showNotify", "()V");
    if (sc) sc(d, RT_SCREEN_W, RT_SCREEN_H);
    if (sn) sn(d);
    repaint_pending = 1;
}
int32_t LCDUI(Display_vibrate__I)(JObj *self, int32_t ms) { (void)self; (void)ms; CNT("Display.vibrate"); return 0; }

/* ------------------------------------------------ Displayable / Canvas */
void LCDUI(Displayable_setCommandListener__Ljavax_microedition_lcdui_CommandListener)(JObj *s, JObj *l) { (void)l; NN(s); CNT("Displayable.setCommandListener"); }
void LCDUI(Displayable_addCommand__Ljavax_microedition_lcdui_Command)(JObj *s, JObj *c) { (void)c; NN(s); CNT("Displayable.addCommand"); }
void LCDUI(Displayable_removeCommand__Ljavax_microedition_lcdui_Command)(JObj *s, JObj *c) { (void)c; NN(s); CNT("Displayable.removeCommand"); }
void LCDUI(Canvas_init__)(JObj *self) { (void)self; }
void LCDUI(Canvas_setFullScreenMode__Z)(JObj *self, int32_t f) { (void)f; NN(self); CNT("Canvas.setFullScreenMode"); }
void LCDUI(Canvas_repaint__)(JObj *self) { NN(self); repaint_pending = 1; CNT("Canvas.repaint"); }

void rt_canvas_service(void) {
    JObj *cv = display.current; if (!cv || !repaint_pending) return;
    void (*paint)(JObj *, JObj *) = jvm_find_method(cv->cls, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
    if (!paint) return;
    pthread_mutex_lock(&paint_mu);
    if (repaint_pending) {
        repaint_pending = 0;
        if (!screen) { JObj *i = LCDUI(Image_createImage__I_I)(RT_SCREEN_W, RT_SCREEN_H); screen = (JImage *)i; screen_g = gfx_new_graphics(screen); }
        screen_g->cx = 0; screen_g->cy = 0; screen_g->cw = RT_SCREEN_W; screen_g->ch = RT_SCREEN_H;
        JvmTry *saved = jvm_try_top; JvmTry f; f.prev = saved; jvm_try_top = &f;
        if (!setjmp(f.jb)) { paint(cv, &screen_g->o); jvm_try_top = saved; }
        else {
            jvm_try_top = saved; JThrowable *ex = (JThrowable *)jvm_exc; char *m = ex && ex->msg ? jstr_dup_utf8(ex->msg) : NULL;
            fprintf(stderr, "[paint] uncaught %s%s%s\n", ex ? ex->o.cls->name : "?", m ? ": " : "", m ? m : ""); free(m);
        }
        rt_count("frames.painted"); frame_counter++;
    }
    pthread_mutex_unlock(&paint_mu);
}
void LCDUI(Canvas_serviceRepaints__)(JObj *self) { NN(self); CNT("Canvas.serviceRepaints"); rt_canvas_service(); }

/* ---------------------------------------------------------- Command / Font */
JObj *rt_new_javax_microedition_lcdui_Command(void) { return rt_alloc_obj(&rt_Command_class); }
void LCDUI(Command_init__Ljava_lang_String_I_I)(JObj *self, JObj *l, int32_t t, int32_t p) { JCommand *c = (JCommand *)NN(self); c->label = l; c->type = t; c->prio = p; }
JObj *LCDUI(Font_getFont__I_I_I)(int32_t face, int32_t style, int32_t size) {
    JFont *f = (JFont *)rt_alloc_obj(&rt_Font_class); f->face = face; f->style = style; f->size = size; return &f->o;
}
int32_t LCDUI(Font_getHeight__)(JObj *self) {
    JFont *f = (JFont *)NN(self); return f->size == 8 ? 12 : f->size == 16 ? 18 : 14;   /* SMALL/LARGE/MEDIUM placeholder metrics */
}


/* ----------------------------------------------- front-end interface */
uint64_t rt_frame_counter(void) { return frame_counter; }
int rt_screen_snapshot(uint32_t *out) {      /* ARGB, RT_SCREEN_W*RT_SCREEN_H; 0 if nothing painted yet */
    pthread_mutex_lock(&paint_mu);
    int ok = screen != NULL;
    if (ok) memcpy(out, screen->pix, sizeof(uint32_t) * RT_SCREEN_W * RT_SCREEN_H);
    pthread_mutex_unlock(&paint_mu);
    return ok;
}
void rt_key_event(int pressed, int32_t keycode) {
    JObj *cv = display.current; if (!cv) return;
    void (*fn)(JObj *, int32_t) = jvm_find_method(cv->cls, pressed ? "keyPressed" : "keyReleased", "(I)V");
    if (!fn) return;
    JvmTry *saved = jvm_try_top; JvmTry f; f.prev = saved; jvm_try_top = &f;
    if (!setjmp(f.jb)) { fn(cv, keycode); jvm_try_top = saved; }
    else { jvm_try_top = saved; fprintf(stderr, "[key] uncaught exception in key handler\n"); }
}
