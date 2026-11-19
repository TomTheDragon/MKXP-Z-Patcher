/* MKXP-Z for the 3DS (experiment): a small RGSS engine.
 *
 * mkxp-z draws with OpenGL shaders, which the 3DS doesn't have, so this is a
 * separate engine: Ruby 3.1 (mkxp-z's) with the RGSS classes written for the
 * 3DS. Everything is drawn in software at the game's resolution and scaled to
 * the top screen. */
#ifndef MKXPZ_3DS_ENGINE_H
#define MKXPZ_3DS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <ruby.h>

/* pixels: R, G, B, A bytes (little-endian 0xAABBGGRR), straight alpha */
typedef uint32_t px_t;
#define PX(r, g, b, a) ((px_t) (r) | ((px_t) (g) << 8) | ((px_t) (b) << 16) | ((px_t) (a) << 24))
#define PX_R(p) ((p) & 0xff)
#define PX_G(p) (((p) >> 8) & 0xff)
#define PX_B(p) (((p) >> 16) & 0xff)
#define PX_A(p) ((p) >> 24)

/* --- Rect, Color, Tone, Table (types.c) */
typedef struct { int x, y, w, h; } Rect;
typedef struct { double r, g, b, a; } Color;
typedef struct { double r, g, b, gray; } Tone;
typedef struct {
	int dim, xs, ys, zs;
	int16_t *data;
	unsigned version; /* changes with every write (tilemap caches) */
} Table;

extern VALUE cRect, cColor, cTone, cTable;
Rect *rect_get(VALUE v);
Color *color_get(VALUE v);
Tone *tone_get(VALUE v);
Table *table_get(VALUE v);
VALUE rect_new(int x, int y, int w, int h);
VALUE color_new(double r, double g, double b, double a);
VALUE tone_new(double r, double g, double b, double gray);
void init_types(void);

/* --- Bitmap and Font (bitmap.c, font.c) */
typedef struct {
	int w, h;
	px_t *px;
	VALUE font;
	bool disposed;
	unsigned version; /* changes with every drawing (for caches) */
	void *gpu;        /* its texture (gpu.c) */
	unsigned gpu_version;
} Bitmap;

extern VALUE cBitmap, cFont;
Bitmap *bitmap_get(VALUE v); /* raises if disposed */
Bitmap *bitmap_get_maybe(VALUE v); /* NULL for nil or disposed */
VALUE bitmap_new_size(int w, int h);
void bitmap_blt(Bitmap *dst, int dx, int dy, Bitmap *src, Rect sr, int opacity);
void init_bitmap(void);
void init_font(void);
/* text into a bitmap (font.c) */
void font_draw_text(Bitmap *b, VALUE font, int x, int y, int w, int h, VALUE str, int align);
void font_text_size(VALUE font, VALUE str, int *w, int *h);

/* --- drawing onto the screen buffer (scene.c) */
typedef struct {
	px_t *px;          /* software drawing; NULL: the GPU */
	int w, h, stride;
	Rect clip;
	bool gpu;
	const Tone *tone;  /* GPU: the viewport's tone, applied to what's drawn */
} Canvas;

/* draw part of a bitmap: position, zoom, mirror, opacity, blend type,
 * color and tone (NULL: none) */
typedef struct {
	int x, y;          /* where the source rect's top left goes */
	double zx, zy;
	bool mirror;
	int opacity;       /* 0..255 */
	int blend;         /* 0 normal, 1 add, 2 subtract */
	const Color *color;
	const Tone *tone;
	int bush_depth, bush_opacity;
	double angle;      /* GPU only: rotation around (cx, cy) */
	float cx, cy;
} DrawOpts;
void canvas_draw(Canvas *c, Bitmap *src, Rect sr, const DrawOpts *o);
void canvas_effect(Canvas *c, Rect r, const Color *color, const Tone *tone);
void canvas_fill(Canvas *c, Rect r, px_t p);

/* gpu.c */
extern bool gpu_on;
void gpu_init(void);
void gpu_set_screen(int w, int h);
void gpu_draw(Canvas *c, Bitmap *src, Rect sr, const DrawOpts *o);
void gpu_fill(Canvas *c, Rect r, const Color *col, int alpha);
void gpu_forget(Bitmap *b);
void gpu_frame_begin(void);
void gpu_scene_begin(int which); /* 0: the picture, 1: the frozen picture */
void gpu_present(int frozen_alpha, int brightness);
void gpu_frame_end(void);
bool gpu_tiles_begin(Canvas *c, Bitmap *b, int opacity);
/* vertex buffers (tilemaps): 9 floats per vertex (x y z, u v, r g b a) */
void gpu_untextured_begin(Canvas *c);
void gpu_draw_buffer(const float *verts, int first, int count, float tx, float ty);
void *gpu_alloc(size_t bytes);
void gpu_free(void *p);
void gpu_flush(const void *p, size_t bytes);
void gpu_tile(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);

/* scene elements: sprites, planes, windows, tilemaps; inside a viewport or
 * on the screen. Sorted by z, then by creation. */
typedef struct Element Element;
struct Element {
	int z;
	unsigned id;
	VALUE self;
	VALUE viewport;   /* Viewport or nil */
	bool is_viewport;
	int z_offset;     /* tilemap layers above characters */
	void (*draw)(Element *e, Canvas *c, int ox, int oy);
	void (*draw_z)(Element *e, Canvas *c, int ox, int oy, int layer);
	Element *next;    /* registry */
};
void element_register(Element *e, VALUE self, VALUE viewport);
void element_unregister(Element *e);
void scene_draw(Canvas *c);

extern VALUE cViewport, cSprite, cPlane, cWindow, cTilemap;
void init_scene(void);
void init_window(void);
void init_tilemap(void);

/* --- Graphics, Input, Audio, Kernel */
extern int screen_w, screen_h;
void init_graphics(void);
void init_input(void);
void init_audio(void);
void init_kernel(void);
void input_poll(void); /* once per frame, from Graphics.update */
void engine_check_reset(void);

/* --- helpers */
int num2int(VALUE v);
double num2dbl(VALUE v);
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define DEF_ATTR(klass, name, getter, setter) \
	rb_define_method(klass, name, getter, 0); \
	rb_define_method(klass, name "=", setter, 1)

extern VALUE eRGSSError, eRGSSReset;
extern int rgss_version; /* 1 XP, 2 VX, 3 VX Ace */

#endif
