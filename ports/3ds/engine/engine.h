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
typedef struct Bitmap_ {
	int w, h;
	px_t *px;
	VALUE font;
	bool disposed;
	unsigned version; /* changes with every drawing (for caches) */
	void *gpu;        /* its texture (gpu.c) */
	unsigned gpu_version;
	/* loaded from a file: while unchanged (version == file_version), its
	 * pixels can be dropped when memory runs low and read again */
	char *file;
	unsigned file_version;
	bool dropped;
	struct Bitmap_ *lru_prev, *lru_next;
	/* window contents with sharp text (settings: "Window text"): draw_text
	 * only records the text; it's drawn at the top screen's resolution on
	 * the finished picture (gpu.c), from a layer with only the text */
	bool text_native;
	struct TextOp *ops;
	int nops, ops_cap;
	unsigned ops_gen;        /* changes when texts are removed */
	struct Bitmap_ *layer;   /* the text at screen size (C only, no Ruby object) */
	unsigned layer_gen;
	int layer_done;          /* ops already in the layer */
	float layer_sx, layer_sy;
} Bitmap;
typedef struct TextOp { Rect r; int align; VALUE str, font; } TextOp;
/* the text layer at scale sx, sy (NULL: none); full: also when not native */
Bitmap *bitmap_text_layer(Bitmap *b, float sx, float sy);
void bitmap_text_layers_drop_full(void); /* the scale-1 layers made for a freeze */
void font_draw_text_scaled(Bitmap *b, VALUE font, int x, int y, int w, int h, VALUE str, int align, float scale);

extern VALUE cBitmap, cFont;
Bitmap *bitmap_get(VALUE v); /* raises if disposed */
px_t *bitmap_px(Bitmap *b); /* its pixels (read again if dropped); NULL: no memory */
void bitmap_uploaded(Bitmap *b); /* gpu.c: its texture is up to date */
extern unsigned bitmap_released;
size_t heap_free(void);     /* free normal memory (memory.c) */
/* memory.c: pixel buffers (64 KB and more in an arena of their own) */
#define PX_MAP_MIN (64 * 1024)
void *px_alloc(size_t bytes, bool zero);
void *px_alloc_at(size_t bytes, bool zero, bool keep); /* keep: from the arena's top */
void px_free(void *p, size_t bytes);
bool px_in_arena(const void *p);
void mem_init(void);
void mem_tell_ruby(void); /* (at safe points: may run Ruby's garbage collector) */
size_t mem_arena_free(void);
bool mem_release(size_t need, bool in_arena); /* bitmap.c */
extern unsigned mem_px_maps;
extern size_t mem_px_bytes;
extern size_t mem_stack_bytes;
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
/* fit with wide_text: the picture is this much wider on each side than
 * the game's screen (the sides show only elements marked mkxpz_wide) */
extern int gpu_margin;
void gpu_draw(Canvas *c, Bitmap *src, Rect sr, const DrawOpts *o);
void gpu_fill(Canvas *c, Rect r, const Color *col, int alpha);
void gpu_forget(Bitmap *b);
void gpu_frame_begin(void);
void gpu_frame_begin_sync(bool sync);
void gpu_scene_begin(int which); /* 0: the picture, 1: the frozen picture */
void gpu_present(int frozen_alpha, int brightness);
void gpu_frame_end(void);
/* window.c: a window's contents have recorded texts: drawn sharp on the
 * finished picture (gx, gy: the contents' origin, clip: what shows, in
 * game pixels) */
void gpu_text_layer(Bitmap *cont, int gx, int gy, Rect clip, int opacity);
bool gpu_tiles_begin(Canvas *c, Bitmap *b, int opacity);
bool gpu_plane(Canvas *c, Bitmap *b, int bx, int by, double zx, double zy, int opacity, int blend,
               const Color *color, const Tone *tone);
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
	bool wide;        /* mkxpz_wide: may use the screen's sides (gpu_margin) */
	void (*draw)(Element *e, Canvas *c, int ox, int oy);
	void (*draw_z)(Element *e, Canvas *c, int ox, int oy, int layer);
	Element *next;    /* registry */
};
void element_register(Element *e, VALUE self, VALUE viewport);
void element_unregister(Element *e);
void scene_draw(Canvas *c);

extern VALUE cViewport, cSprite, cPlane, cWindow, cTilemap;
void init_scene(void);
void init_fast(void); /* native Sprite_Character#update (scene.c) */
void init_events(void); /* native Game_Event#refresh (events.c) */
void init_window(void);
void init_tilemap(void);

/* --- Graphics, Input, Audio, Kernel */
extern int screen_w, screen_h;
void init_graphics(void);
void init_input(void);
void init_audio(void);
bool audio_start(void); /* false: no sound (no DSP firmware) */
void audio_stop(void);
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
/* files.c: the game's files, found through cached folder listings */
bool file_find(const char *path, const char *const *exts, char *out, size_t n);

/* settings.c: START in the game */
typedef struct {
	int music_volume, sound_volume; /* 0..100 */
	int buttons;    /* 0: B confirm, A cancel, Y dash; 1: A confirm, B cancel, X dash */
	int screen;     /* 0 fit, 1 stretch */
	int wide_text;  /* fit: message boxes over the screen's whole width */
	int frame_skip; /* 0 off, 1 auto */
	int show_speed;
	int overlay;    /* game speed and memory on the bottom screen */
	int sharp_text; /* window text drawn at the top screen's resolution */
	int colors;     /* 0 normal, 1 vivid (more contrast, for the original 3DS's screen) */
} Settings;
extern Settings settings;
void settings_load(void);
bool settings_menu(void); /* true: reset the game */
void audio_volumes_changed(void);
/* rbinternal.c: Ruby internals */
typedef struct { VALUE klass; uint32_t index; } IvCache;
VALUE mkxpz_ivar(VALUE obj, ID id, IvCache *c);
VALUE mkxpz_gvar(ID id);
VALUE mkxpz_frame_class_name(VALUE frame, bool *singleton);
#define IV(obj, id) ({ static IvCache iv_c_; mkxpz_ivar((obj), (id), &iv_c_); })
int graphics_frame(void);
unsigned stack_used_kb(void); /* main.c: most of the main stack ever used */ /* graphics.c: Graphics.frame_count */
/* profile.c (file mkxpz-profile) */
extern bool prof_on;
void prof_init(void);
void prof_graphics_begin(void);
void prof_graphics_end(void);
void prof_report(int frames);
void prof_caller(char *out, size_t n);
extern unsigned bitmaps_created;
void status_line(const char *text); /* main.c: the bottom screen's first line */
void say(const char *fmt, ...);
void log_only(const char *fmt, ...); /* mkxpz-3ds.log only */ /* the bottom screen and mkxpz-3ds.log */
extern int rgss_version; /* 1 XP, 2 VX, 3 VX Ace */

#endif
