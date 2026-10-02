/* rt.h - runtime-internal declarations (not used by generated code) */
#ifndef RT_H
#define RT_H
#include "jvm.h"
#include <stdio.h>
#include <pthread.h>
#include "jrt_api.h"   /* generated: exact signatures the game expects */

/* Symbol-prefix helpers: JL(String_length__) -> rt_java_lang_String_length__ */
#define JL(n)     rt_java_lang_##n
#define JIO(n)    rt_java_io_##n
#define JU(n)     rt_java_util_##n
#define LCDUI(n)  rt_javax_microedition_lcdui_##n
#define MEDIA(n)  rt_javax_microedition_media_##n
#define MCTL(n)   rt_javax_microedition_media_control_##n
#define RMS(n)    rt_javax_microedition_rms_##n
#define MIDLET(n) rt_javax_microedition_midlet_##n

typedef struct JString    { JObj o; int32_t len; uint16_t *chars; int32_t hash; } JString;
typedef struct JThrowable { JObj o; JObj *msg; } JThrowable;
typedef struct JInteger   { JObj o; int32_t v; } JInteger;

/* ---- runtime class descriptors (defined in core.c unless noted) ---- */
extern JClass rt_Object_class, rt_Class_class, rt_String_class, rt_StringBuffer_class,
    rt_Integer_class, rt_Thread_class, rt_Hashtable_class, rt_Random_class, rt_PrintStream_class,
    rt_InputStream_class, rt_ByteArrayInputStream_class, rt_array_class,
    rt_Throwable_class, rt_Exception_class, rt_RuntimeException_class, rt_Error_class,
    rt_NullPointerException_class, rt_ArithmeticException_class,
    rt_IndexOutOfBoundsException_class, rt_ArrayIndexOutOfBoundsException_class,
    rt_StringIndexOutOfBoundsException_class, rt_NegativeArraySizeException_class,
    rt_ClassCastException_class, rt_IllegalArgumentException_class,
    rt_IOException_class, rt_OutOfMemoryError_class,
    rt_RecordStoreException_class, rt_RecordStoreNotFoundException_class,
    rt_InvalidRecordIDException_class, rt_RecordStoreFullException_class,
    rt_MediaException_class, rt_IllegalStateException_class;
/* defined in their own files */
extern JClass rt_MIDlet_class, rt_Canvas_class, rt_Displayable_class, rt_Display_class,
    rt_Command_class, rt_Graphics_class, rt_Image_class, rt_Font_class,
    rt_RecordStore_class, rt_Player_class, rt_VolumeControl_class;

JClass *rt_class_by_name(const char *name);

/* ---- allocation / exceptions ---- */
void *jvm_alloc(size_t n);                       /* zeroed, never freed (no GC yet) */
JObj *rt_alloc_obj(JClass *c);
JObj *rt_new_exception(JClass *c, const char *msg);
JVM_NORETURN void rt_throw(JClass *c, const char *msg);
#define NN(x) jvm_nn((JObj *)(x))

/* ---- strings ---- */
JObj   *jstr_new(const uint16_t *s, int32_t n);          /* copies */
JObj   *jstr_from_utf8(const char *s);
JObj   *jstr_decode(const uint8_t *b, int32_t n, const char *enc);
char   *jstr_dup_utf8(JObj *s);                          /* malloc'd; NULL for null */
int     jstr_equals(JObj *a, JObj *b);
int32_t jobj_hash(JObj *o);
int     jobj_equals(JObj *a, JObj *b);
JObj   *jstr_value_of(JObj *o);                          /* String.valueOf(Object) */
JObj   *jint_new(int32_t v);

/* ---- host environment (set by main) ---- */
extern const char *rt_res_dir;     /* extracted JAR contents */
extern const char *rt_save_dir;    /* RMS persistence */
extern volatile int rt_quit;

/* ---- resources / streams ---- */
JObj *rt_stream_from_memory(const uint8_t *data, int32_t len);
uint8_t *rt_read_file(const char *path, int32_t *len);   /* malloc'd or NULL */
const char *rt_manifest_prop(const char *key);

/* ---- diagnostics: per-API call counters, printed at exit ---- */
void rt_count(const char *api);
void rt_dump_counts(FILE *f);

/* ---- lcdui (stub) hooks used by the harness ---- */
#define RT_SCREEN_W 240
#define RT_SCREEN_H 320
void rt_canvas_service(void);        /* paint now if a repaint is pending */
JObj *rt_display_current(void);
uint64_t rt_frame_counter(void);
void rt_sleep_set_compensation(int32_t ms);
int32_t rt_sleep_get_compensation(void);
int  rt_screen_snapshot(uint32_t *out);          /* copy of the last painted frame (ARGB) */
void rt_key_event(int pressed, int32_t keycode); /* deliver to Canvas.keyPressed/keyReleased */

/* ---- audio ---- */
#define RT_AUDIO_RATE 44100
void rt_audio_mix(int16_t *out, int frames);          /* stereo interleaved S16; called by the audio device */
void rt_audio_virtual_start(const char *wav_path);    /* headless: real-time virtual device, optional WAV capture */
void rt_audio_virtual_stop(void);
long rt_count_value(const char *api);
#endif
