/* gfx.h - Image / Graphics layouts shared by gfx.c and lcdui.c */
#ifndef GFX_H
#define GFX_H
#include "rt.h"
typedef struct JImage { JObj o; int32_t w, h; uint32_t *pix; int mut; } JImage;   /* ARGB32 */
typedef struct JGraphics { JObj o; JImage *img; int32_t color, cx, cy, cw, ch; JObj *font; } JGraphics;
JGraphics *gfx_new_graphics(JImage *img);
#endif
