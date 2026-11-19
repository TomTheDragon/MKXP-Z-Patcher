/* The screen: everything shown (sprites, planes, windows, tilemaps,
 * viewports) and drawing it in software, plus Viewport, Sprite and Plane. */
#include "engine.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

VALUE cViewport, cSprite, cPlane;

/* --- drawing onto a canvas */

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* color and tone of one pixel (r, g, b in place) */
static inline void effects(int *r, int *g, int *b, const Color *color, const Tone *tone)
{
	if (tone) {
		if (tone->gray > 0) {
			int l = (*r * 77 + *g * 150 + *b * 29) >> 8;
			int gr = (int) tone->gray;
			*r += (l - *r) * gr / 255;
			*g += (l - *g) * gr / 255;
			*b += (l - *b) * gr / 255;
		}
		*r = clamp255(*r + (int) tone->r);
		*g = clamp255(*g + (int) tone->g);
		*b = clamp255(*b + (int) tone->b);
	}
	if (color && color->a > 0) {
		int ca = (int) color->a;
		*r += ((int) color->r - *r) * ca / 255;
		*g += ((int) color->g - *g) * ca / 255;
		*b += ((int) color->b - *b) * ca / 255;
	}
}

static inline px_t blend(px_t d, int r, int g, int b, int a, int mode)
{
	int dr = PX_R(d), dg = PX_G(d), db = PX_B(d);
	switch (mode) {
	case 1: /* add */
		return PX(clamp255(dr + r * a / 255), clamp255(dg + g * a / 255), clamp255(db + b * a / 255), 255);
	case 2: /* subtract */
		return PX(clamp255(dr - r * a / 255), clamp255(dg - g * a / 255), clamp255(db - b * a / 255), 255);
	default:
		return PX(dr + (r - dr) * a / 255, dg + (g - dg) * a / 255, db + (b - db) * a / 255, 255);
	}
}

static bool tone_active(const Tone *t) { return t && (t->r || t->g || t->b || t->gray); }
static bool color_active(const Color *c) { return c && c->a > 0; }

void canvas_draw(Canvas *c, Bitmap *src, Rect sr, const DrawOpts *o)
{
	if (c->gpu) {
		gpu_draw(c, src, sr, o);
		return;
	}
	/* source rect inside the bitmap */
	if (sr.x < 0) { sr.w += sr.x; sr.x = 0; }
	if (sr.y < 0) { sr.h += sr.y; sr.y = 0; }
	if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
	if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
	if (sr.w <= 0 || sr.h <= 0 || o->opacity <= 0 || o->zx <= 0 || o->zy <= 0)
		return;
	int dw = (int) lround(sr.w * o->zx), dh = (int) lround(sr.h * o->zy);
	int x0 = o->x, y0 = o->y;
	int cx0 = x0 > c->clip.x ? x0 : c->clip.x, cy0 = y0 > c->clip.y ? y0 : c->clip.y;
	int cx1 = x0 + dw < c->clip.x + c->clip.w ? x0 + dw : c->clip.x + c->clip.w;
	int cy1 = y0 + dh < c->clip.y + c->clip.h ? y0 + dh : c->clip.y + c->clip.h;
	if (cx0 >= cx1 || cy0 >= cy1)
		return;
	const Color *color = color_active(o->color) ? o->color : NULL;
	const Tone *tone = tone_active(o->tone) ? o->tone : NULL;
	bool plain = !color && !tone && o->blend == 0 && dw == sr.w && dh == sr.h && !o->mirror && o->bush_depth <= 0;
	int bush_row = sr.h - o->bush_depth; /* source rows from here: bush opacity */
	for (int y = cy0; y < cy1; y++) {
		int sy = dh == sr.h ? y - y0 : (int) ((long long) (y - y0) * sr.h / dh);
		const px_t *srow = src->px + (size_t) (sr.y + sy) * src->w + sr.x;
		px_t *drow = c->px + (size_t) y * c->stride;
		int op = o->opacity;
		if (o->bush_depth > 0 && sy >= bush_row)
			op = op * o->bush_opacity / 255;
		if (plain) {
			for (int x = cx0; x < cx1; x++) {
				px_t s = srow[x - x0];
				int a = PX_A(s) * op / 255;
				if (!a)
					continue;
				px_t d = drow[x];
				if (a == 255) {
					drow[x] = s | 0xff000000;
					continue;
				}
				drow[x] = PX(PX_R(d) + (PX_R(s) - (int) PX_R(d)) * a / 255,
				             PX_G(d) + (PX_G(s) - (int) PX_G(d)) * a / 255,
				             PX_B(d) + (PX_B(s) - (int) PX_B(d)) * a / 255, 255);
			}
			continue;
		}
		for (int x = cx0; x < cx1; x++) {
			int sx = dw == sr.w ? x - x0 : (int) ((long long) (x - x0) * sr.w / dw);
			if (o->mirror)
				sx = sr.w - 1 - sx;
			px_t s = srow[sx];
			int a = PX_A(s) * op / 255;
			if (!a)
				continue;
			int r = PX_R(s), g = PX_G(s), b = PX_B(s);
			effects(&r, &g, &b, color, tone);
			drow[x] = blend(drow[x], r, g, b, a, o->blend);
		}
	}
}

void canvas_effect(Canvas *c, Rect r, const Color *color, const Tone *tone)
{
	if (c->gpu) {
		/* (tone: passed on to what's drawn, see Canvas.tone) */
		if (color_active(color))
			gpu_fill(c, r, color, (int) color->a);
		return;
	}
	color = color_active(color) ? color : NULL;
	tone = tone_active(tone) ? tone : NULL;
	if (!color && !tone)
		return;
	int x0 = r.x > c->clip.x ? r.x : c->clip.x, y0 = r.y > c->clip.y ? r.y : c->clip.y;
	int x1 = r.x + r.w < c->clip.x + c->clip.w ? r.x + r.w : c->clip.x + c->clip.w;
	int y1 = r.y + r.h < c->clip.y + c->clip.h ? r.y + r.h : c->clip.y + c->clip.h;
	for (int y = y0; y < y1; y++) {
		px_t *row = c->px + (size_t) y * c->stride;
		for (int x = x0; x < x1; x++) {
			int rr = PX_R(row[x]), g = PX_G(row[x]), b = PX_B(row[x]);
			effects(&rr, &g, &b, color, tone);
			row[x] = PX(rr, g, b, 255);
		}
	}
}

void canvas_fill(Canvas *c, Rect r, px_t p)
{
	if (c->gpu) {
		Color col = { PX_R(p), PX_G(p), PX_B(p), 255 };
		gpu_fill(c, r, &col, PX_A(p));
		return;
	}
	int x0 = r.x > c->clip.x ? r.x : c->clip.x, y0 = r.y > c->clip.y ? r.y : c->clip.y;
	int x1 = r.x + r.w < c->clip.x + c->clip.w ? r.x + r.w : c->clip.x + c->clip.w;
	int y1 = r.y + r.h < c->clip.y + c->clip.h ? r.y + r.h : c->clip.y + c->clip.h;
	int a = PX_A(p);
	for (int y = y0; y < y1; y++) {
		px_t *row = c->px + (size_t) y * c->stride;
		for (int x = x0; x < x1; x++)
			row[x] = a == 255 ? p : blend(row[x], PX_R(p), PX_G(p), PX_B(p), a, 0);
	}
}

/* --- the registry of shown elements */

static Element *elements;
static unsigned next_id;

void element_register(Element *e, VALUE self, VALUE viewport)
{
	e->self = self;
	e->viewport = viewport;
	e->id = next_id++;
	e->next = elements;
	elements = e;
}

void element_unregister(Element *e)
{
	for (Element **p = &elements; *p; p = &(*p)->next)
		if (*p == e) {
			*p = e->next;
			e->next = NULL;
			return;
		}
}

static int by_z(const void *a, const void *b)
{
	const Element *x = *(Element *const *) a, *y = *(Element *const *) b;
	if (x->z != y->z)
		return x->z < y->z ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

static Element **collect(VALUE viewport, int *n)
{
	int count = 0;
	for (Element *e = elements; e; e = e->next)
		if (e->viewport == viewport)
			count++;
	Element **list = malloc(sizeof *list * (count ? count : 1));
	int i = 0;
	for (Element *e = elements; e; e = e->next)
		if (e->viewport == viewport)
			list[i++] = e;
	qsort(list, count, sizeof *list, by_z);
	*n = count;
	return list;
}

/* --- Viewport */
typedef struct {
	Element el;
	VALUE rect, color, tone;
	bool visible, disposed;
	int ox, oy;
	Color flash;
	int flash_left, flash_dur;
} Viewport;

static void viewport_draw(Element *e, Canvas *c, int ox, int oy);

static void viewport_mark(void *p)
{
	Viewport *v = p;
	rb_gc_mark(v->rect);
	rb_gc_mark(v->color);
	rb_gc_mark(v->tone);
}
static void viewport_free(void *p)
{
	Viewport *v = p;
	if (!v->disposed)
		element_unregister(&v->el);
	xfree(v);
}
static const rb_data_type_t viewport_type = { "Viewport", { viewport_mark, viewport_free, NULL }, 0, 0, 0 };
static VALUE viewport_alloc(VALUE klass)
{
	Viewport *v;
	VALUE self = TypedData_Make_Struct(klass, Viewport, &viewport_type, v);
	v->rect = v->color = v->tone = Qnil;
	return self;
}
static Viewport *viewport_get(VALUE self)
{
	Viewport *v = rb_check_typeddata(self, &viewport_type);
	if (v->disposed)
		rb_raise(eRGSSError, "disposed viewport");
	return v;
}

static VALUE viewport_init(int argc, VALUE *argv, VALUE self)
{
	Viewport *v = rb_check_typeddata(self, &viewport_type);
	if (argc == 0)
		v->rect = rect_new(0, 0, screen_w, screen_h);
	else if (argc == 1)
		v->rect = rect_new(rect_get(argv[0])->x, rect_get(argv[0])->y, rect_get(argv[0])->w, rect_get(argv[0])->h);
	else if (argc == 4)
		v->rect = rect_new(num2int(argv[0]), num2int(argv[1]), num2int(argv[2]), num2int(argv[3]));
	else
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 0, 1 or 4)", argc);
	v->color = color_new(0, 0, 0, 0);
	v->tone = tone_new(0, 0, 0, 0);
	v->visible = true;
	v->el.is_viewport = true;
	v->el.draw = viewport_draw;
	element_register(&v->el, self, Qnil);
	return self;
}

static void viewport_draw(Element *e, Canvas *c, int ox, int oy)
{
	Viewport *v = (Viewport *) e;
	if (!v->visible)
		return;
	Rect r = *rect_get(v->rect);
	Canvas sub = *c;
	int x0 = r.x > c->clip.x ? r.x : c->clip.x, y0 = r.y > c->clip.y ? r.y : c->clip.y;
	int x1 = r.x + r.w < c->clip.x + c->clip.w ? r.x + r.w : c->clip.x + c->clip.w;
	int y1 = r.y + r.h < c->clip.y + c->clip.h ? r.y + r.h : c->clip.y + c->clip.h;
	if (x0 >= x1 || y0 >= y1)
		return;
	sub.clip = (Rect) { x0, y0, x1 - x0, y1 - y0 };
	if (c->gpu)
		sub.tone = tone_get(v->tone);
	int n;
	Element **list = collect(e->self, &n);
	for (int i = 0; i < n; i++)
		list[i]->draw(list[i], &sub, r.x - v->ox, r.y - v->oy);
	free(list);
	canvas_effect(&sub, sub.clip, color_get(v->color), c->gpu ? NULL : tone_get(v->tone));
	if (v->flash_left > 0) {
		Color f = v->flash;
		f.a = f.a * v->flash_left / v->flash_dur;
		canvas_effect(&sub, sub.clip, &f, NULL);
	}
}

static VALUE viewport_dispose(VALUE self)
{
	Viewport *v = rb_check_typeddata(self, &viewport_type);
	if (!v->disposed) {
		v->disposed = true;
		element_unregister(&v->el);
	}
	return Qnil;
}
static VALUE viewport_disposed(VALUE self) { return ((Viewport *) rb_check_typeddata(self, &viewport_type))->disposed ? Qtrue : Qfalse; }
static VALUE viewport_rect(VALUE self) { return viewport_get(self)->rect; }
static VALUE viewport_set_rect(VALUE self, VALUE r) { *rect_get(viewport_get(self)->rect) = *rect_get(r); return r; }
static VALUE viewport_color(VALUE self) { return viewport_get(self)->color; }
static VALUE viewport_set_color(VALUE self, VALUE c) { *color_get(viewport_get(self)->color) = *color_get(c); return c; }
static VALUE viewport_tone(VALUE self) { return viewport_get(self)->tone; }
static VALUE viewport_set_tone(VALUE self, VALUE t) { *tone_get(viewport_get(self)->tone) = *tone_get(t); return t; }
static VALUE viewport_visible(VALUE self) { return viewport_get(self)->visible ? Qtrue : Qfalse; }
static VALUE viewport_set_visible(VALUE self, VALUE b) { viewport_get(self)->visible = RTEST(b); return b; }
static VALUE viewport_z(VALUE self) { return INT2NUM(viewport_get(self)->el.z); }
static VALUE viewport_set_z(VALUE self, VALUE z) { viewport_get(self)->el.z = num2int(z); return z; }
static VALUE viewport_ox(VALUE self) { return INT2NUM(viewport_get(self)->ox); }
static VALUE viewport_set_ox(VALUE self, VALUE v) { viewport_get(self)->ox = num2int(v); return v; }
static VALUE viewport_oy(VALUE self) { return INT2NUM(viewport_get(self)->oy); }
static VALUE viewport_set_oy(VALUE self, VALUE v) { viewport_get(self)->oy = num2int(v); return v; }
static VALUE viewport_flash(VALUE self, VALUE color, VALUE dur)
{
	Viewport *v = viewport_get(self);
	v->flash = NIL_P(color) ? (Color) { 0, 0, 0, 255 } : *color_get(color);
	v->flash_dur = v->flash_left = num2int(dur);
	return Qnil;
}
static VALUE viewport_update(VALUE self)
{
	Viewport *v = viewport_get(self);
	if (v->flash_left > 0)
		v->flash_left--;
	return Qnil;
}

/* --- Sprite */
typedef struct {
	Element el;
	VALUE bitmap, src_rect, color, tone;
	bool visible, mirror, disposed;
	int x, y, ox, oy, opacity, blend_type, bush_depth, bush_opacity;
	double zoom_x, zoom_y, angle;
	Color flash;
	bool flash_hide;
	int flash_left, flash_dur;
	int wave_amp, wave_length, wave_speed;
	double wave_phase;
} Sprite;

static void sprite_draw(Element *e, Canvas *c, int ox, int oy);

static void sprite_mark(void *p)
{
	Sprite *s = p;
	rb_gc_mark(s->bitmap);
	rb_gc_mark(s->src_rect);
	rb_gc_mark(s->color);
	rb_gc_mark(s->tone);
	rb_gc_mark(s->el.viewport);
}
static void sprite_free(void *p)
{
	Sprite *s = p;
	if (!s->disposed)
		element_unregister(&s->el);
	xfree(s);
}
static const rb_data_type_t sprite_type = { "Sprite", { sprite_mark, sprite_free, NULL }, 0, 0, 0 };
static VALUE sprite_alloc(VALUE klass)
{
	Sprite *s;
	VALUE self = TypedData_Make_Struct(klass, Sprite, &sprite_type, s);
	s->bitmap = s->src_rect = s->color = s->tone = Qnil;
	s->el.viewport = Qnil;
	return self;
}
static Sprite *sprite_get(VALUE self)
{
	Sprite *s = rb_check_typeddata(self, &sprite_type);
	if (s->disposed)
		rb_raise(eRGSSError, "disposed sprite");
	return s;
}

static VALUE sprite_init(int argc, VALUE *argv, VALUE self)
{
	Sprite *s = rb_check_typeddata(self, &sprite_type);
	VALUE vp = argc > 0 ? argv[0] : Qnil;
	s->src_rect = rect_new(0, 0, 0, 0);
	s->color = color_new(0, 0, 0, 0);
	s->tone = tone_new(0, 0, 0, 0);
	s->visible = true;
	s->opacity = 255;
	s->bush_opacity = 128;
	s->zoom_x = s->zoom_y = 1.0;
	s->el.draw = sprite_draw;
	element_register(&s->el, self, vp);
	return self;
}

static void sprite_draw(Element *e, Canvas *c, int ox, int oy)
{
	Sprite *s = (Sprite *) e;
	if (!s->visible || s->opacity <= 0 || (s->flash_hide && s->flash_left > 0))
		return;
	Bitmap *b = bitmap_get_maybe(s->bitmap);
	if (!b)
		return;
	Rect sr = *rect_get(s->src_rect);
	Color col = *color_get(s->color);
	if (s->flash_left > 0 && !s->flash_hide) {
		/* the flash fades out; it wins over the sprite's own color while stronger */
		double fa = s->flash.a * s->flash_left / s->flash_dur;
		if (fa > col.a) {
			col = s->flash;
			col.a = fa;
		}
	}
	DrawOpts o = {
		.x = ox + s->x - (int) lround(s->ox * s->zoom_x),
		.y = oy + s->y - (int) lround(s->oy * s->zoom_y),
		.zx = s->zoom_x, .zy = s->zoom_y, .mirror = s->mirror,
		.opacity = s->opacity, .blend = s->blend_type,
		.color = &col, .tone = tone_get(s->tone),
		.bush_depth = s->bush_depth, .bush_opacity = s->bush_opacity,
		.angle = s->angle, .cx = (float) (ox + s->x), .cy = (float) (oy + s->y),
	};
	canvas_draw(c, b, sr, &o);
}

static VALUE sprite_dispose(VALUE self)
{
	Sprite *s = rb_check_typeddata(self, &sprite_type);
	if (!s->disposed) {
		s->disposed = true;
		element_unregister(&s->el);
	}
	return Qnil;
}
static VALUE sprite_disposed(VALUE self) { return ((Sprite *) rb_check_typeddata(self, &sprite_type))->disposed ? Qtrue : Qfalse; }
static VALUE sprite_bitmap(VALUE self) { return sprite_get(self)->bitmap; }
static VALUE sprite_set_bitmap(VALUE self, VALUE b)
{
	Sprite *s = sprite_get(self);
	s->bitmap = b;
	Bitmap *bm = bitmap_get_maybe(b);
	if (bm)
		*rect_get(s->src_rect) = (Rect) { 0, 0, bm->w, bm->h };
	return b;
}
static VALUE sprite_src_rect(VALUE self) { return sprite_get(self)->src_rect; }
static VALUE sprite_set_src_rect(VALUE self, VALUE r) { *rect_get(sprite_get(self)->src_rect) = *rect_get(r); return r; }
static VALUE sprite_color(VALUE self) { return sprite_get(self)->color; }
static VALUE sprite_set_color(VALUE self, VALUE c) { *color_get(sprite_get(self)->color) = *color_get(c); return c; }
static VALUE sprite_tone(VALUE self) { return sprite_get(self)->tone; }
static VALUE sprite_set_tone(VALUE self, VALUE t) { *tone_get(sprite_get(self)->tone) = *tone_get(t); return t; }
static VALUE sprite_viewport(VALUE self) { return sprite_get(self)->el.viewport; }
static VALUE sprite_set_viewport(VALUE self, VALUE v) { sprite_get(self)->el.viewport = v; return v; }
static VALUE sprite_z(VALUE self) { return INT2NUM(sprite_get(self)->el.z); }
static VALUE sprite_set_z(VALUE self, VALUE z) { sprite_get(self)->el.z = num2int(z); return z; }
#define S_INT(f, lo, hi) \
	static VALUE sprite_get_##f(VALUE self) { return INT2NUM(sprite_get(self)->f); } \
	static VALUE sprite_set_##f(VALUE self, VALUE v) { int n = num2int(v); sprite_get(self)->f = CLAMP(n, lo, hi); return v; }
S_INT(x, -999999, 999999) S_INT(y, -999999, 999999) S_INT(ox, -999999, 999999) S_INT(oy, -999999, 999999)
S_INT(opacity, 0, 255) S_INT(blend_type, 0, 2) S_INT(bush_depth, 0, 999999) S_INT(bush_opacity, 0, 255)
S_INT(wave_amp, 0, 999999) S_INT(wave_length, 0, 999999) S_INT(wave_speed, 0, 999999)
#define S_DBL(f) \
	static VALUE sprite_get_##f(VALUE self) { return DBL2NUM(sprite_get(self)->f); } \
	static VALUE sprite_set_##f(VALUE self, VALUE v) { sprite_get(self)->f = num2dbl(v); return v; }
S_DBL(zoom_x) S_DBL(zoom_y) S_DBL(angle) S_DBL(wave_phase)
#define S_BOOL(f) \
	static VALUE sprite_get_##f(VALUE self) { return sprite_get(self)->f ? Qtrue : Qfalse; } \
	static VALUE sprite_set_##f(VALUE self, VALUE v) { sprite_get(self)->f = RTEST(v); return v; }
S_BOOL(visible) S_BOOL(mirror)
static VALUE sprite_width(VALUE self) { return INT2NUM(rect_get(sprite_get(self)->src_rect)->w); }
static VALUE sprite_height(VALUE self) { return INT2NUM(rect_get(sprite_get(self)->src_rect)->h); }
static VALUE sprite_flash(VALUE self, VALUE color, VALUE dur)
{
	Sprite *s = sprite_get(self);
	s->flash_hide = NIL_P(color);
	if (!s->flash_hide)
		s->flash = *color_get(color);
	s->flash_dur = s->flash_left = num2int(dur);
	return Qnil;
}
static VALUE sprite_update(VALUE self)
{
	Sprite *s = sprite_get(self);
	if (s->flash_left > 0)
		s->flash_left--;
	s->wave_phase += s->wave_speed / 180.0;
	return Qnil;
}

/* --- Plane: a bitmap repeated over its viewport (or the screen) */
typedef struct {
	Element el;
	VALUE bitmap, color, tone;
	bool visible, disposed;
	int ox, oy, opacity, blend_type;
	double zoom_x, zoom_y;
} Plane;

static void plane_draw(Element *e, Canvas *c, int ox, int oy);

static void plane_mark(void *p)
{
	Plane *s = p;
	rb_gc_mark(s->bitmap);
	rb_gc_mark(s->color);
	rb_gc_mark(s->tone);
	rb_gc_mark(s->el.viewport);
}
static void plane_free(void *p)
{
	Plane *s = p;
	if (!s->disposed)
		element_unregister(&s->el);
	xfree(s);
}
static const rb_data_type_t plane_type = { "Plane", { plane_mark, plane_free, NULL }, 0, 0, 0 };
static VALUE plane_alloc(VALUE klass)
{
	Plane *s;
	VALUE self = TypedData_Make_Struct(klass, Plane, &plane_type, s);
	s->bitmap = s->color = s->tone = Qnil;
	s->el.viewport = Qnil;
	return self;
}
static Plane *plane_get(VALUE self)
{
	Plane *s = rb_check_typeddata(self, &plane_type);
	if (s->disposed)
		rb_raise(eRGSSError, "disposed plane");
	return s;
}
static VALUE plane_init(int argc, VALUE *argv, VALUE self)
{
	Plane *s = rb_check_typeddata(self, &plane_type);
	s->color = color_new(0, 0, 0, 0);
	s->tone = tone_new(0, 0, 0, 0);
	s->visible = true;
	s->opacity = 255;
	s->zoom_x = s->zoom_y = 1.0;
	s->el.draw = plane_draw;
	element_register(&s->el, self, argc > 0 ? argv[0] : Qnil);
	return self;
}
static void plane_draw(Element *e, Canvas *c, int ox, int oy)
{
	Plane *s = (Plane *) e;
	Bitmap *b = bitmap_get_maybe(s->bitmap);
	if (!s->visible || s->opacity <= 0 || !b)
		return;
	int tw = (int) lround(b->w * s->zoom_x), th = (int) lround(b->h * s->zoom_y);
	if (tw <= 0 || th <= 0)
		return;
	/* tiles covering the clip area; the plane's origin moves with ox/oy */
	int bx = ox - s->ox, by = oy - s->oy; /* where one tile's top left is */
	int sx = c->clip.x + ((bx - c->clip.x) % tw + tw) % tw - tw;
	int sy = c->clip.y + ((by - c->clip.y) % th + th) % th - th;
	DrawOpts o = { .zx = s->zoom_x, .zy = s->zoom_y, .opacity = s->opacity, .blend = s->blend_type,
	               .color = color_get(s->color), .tone = tone_get(s->tone) };
	for (int y = sy; y < c->clip.y + c->clip.h; y += th)
		for (int x = sx; x < c->clip.x + c->clip.w; x += tw) {
			o.x = x;
			o.y = y;
			canvas_draw(c, b, (Rect) { 0, 0, b->w, b->h }, &o);
		}
}
static VALUE plane_dispose(VALUE self)
{
	Plane *s = rb_check_typeddata(self, &plane_type);
	if (!s->disposed) {
		s->disposed = true;
		element_unregister(&s->el);
	}
	return Qnil;
}
static VALUE plane_disposed(VALUE self) { return ((Plane *) rb_check_typeddata(self, &plane_type))->disposed ? Qtrue : Qfalse; }
static VALUE plane_bitmap(VALUE self) { return plane_get(self)->bitmap; }
static VALUE plane_set_bitmap(VALUE self, VALUE b) { plane_get(self)->bitmap = b; return b; }
static VALUE plane_color(VALUE self) { return plane_get(self)->color; }
static VALUE plane_set_color(VALUE self, VALUE c) { *color_get(plane_get(self)->color) = *color_get(c); return c; }
static VALUE plane_tone(VALUE self) { return plane_get(self)->tone; }
static VALUE plane_set_tone(VALUE self, VALUE t) { *tone_get(plane_get(self)->tone) = *tone_get(t); return t; }
static VALUE plane_viewport(VALUE self) { return plane_get(self)->el.viewport; }
static VALUE plane_set_viewport(VALUE self, VALUE v) { plane_get(self)->el.viewport = v; return v; }
static VALUE plane_z(VALUE self) { return INT2NUM(plane_get(self)->el.z); }
static VALUE plane_set_z(VALUE self, VALUE z) { plane_get(self)->el.z = num2int(z); return z; }
#define P_INT(f, lo, hi) \
	static VALUE plane_get_##f(VALUE self) { return INT2NUM(plane_get(self)->f); } \
	static VALUE plane_set_##f(VALUE self, VALUE v) { int n = num2int(v); plane_get(self)->f = CLAMP(n, lo, hi); return v; }
P_INT(ox, -9999999, 9999999) P_INT(oy, -9999999, 9999999) P_INT(opacity, 0, 255) P_INT(blend_type, 0, 2)
static VALUE plane_zoom_x(VALUE self) { return DBL2NUM(plane_get(self)->zoom_x); }
static VALUE plane_set_zoom_x(VALUE self, VALUE v) { plane_get(self)->zoom_x = num2dbl(v); return v; }
static VALUE plane_zoom_y(VALUE self) { return DBL2NUM(plane_get(self)->zoom_y); }
static VALUE plane_set_zoom_y(VALUE self, VALUE v) { plane_get(self)->zoom_y = num2dbl(v); return v; }
static VALUE plane_visible(VALUE self) { return plane_get(self)->visible ? Qtrue : Qfalse; }
static VALUE plane_set_visible(VALUE self, VALUE v) { plane_get(self)->visible = RTEST(v); return v; }

/* --- the whole screen */
void scene_draw(Canvas *c)
{
	int n;
	Element **list = collect(Qnil, &n);
	for (int i = 0; i < n; i++)
		list[i]->draw(list[i], c, 0, 0);
	free(list);
}

void init_scene(void)
{
	cViewport = rb_define_class("Viewport", rb_cObject);
	rb_define_alloc_func(cViewport, viewport_alloc);
	rb_define_method(cViewport, "initialize", viewport_init, -1);
	rb_define_method(cViewport, "dispose", viewport_dispose, 0);
	rb_define_method(cViewport, "disposed?", viewport_disposed, 0);
	rb_define_method(cViewport, "flash", viewport_flash, 2);
	rb_define_method(cViewport, "update", viewport_update, 0);
	DEF_ATTR(cViewport, "rect", viewport_rect, viewport_set_rect);
	DEF_ATTR(cViewport, "color", viewport_color, viewport_set_color);
	DEF_ATTR(cViewport, "tone", viewport_tone, viewport_set_tone);
	DEF_ATTR(cViewport, "visible", viewport_visible, viewport_set_visible);
	DEF_ATTR(cViewport, "z", viewport_z, viewport_set_z);
	DEF_ATTR(cViewport, "ox", viewport_ox, viewport_set_ox);
	DEF_ATTR(cViewport, "oy", viewport_oy, viewport_set_oy);

	cSprite = rb_define_class("Sprite", rb_cObject);
	rb_define_alloc_func(cSprite, sprite_alloc);
	rb_define_method(cSprite, "initialize", sprite_init, -1);
	rb_define_method(cSprite, "dispose", sprite_dispose, 0);
	rb_define_method(cSprite, "disposed?", sprite_disposed, 0);
	rb_define_method(cSprite, "flash", sprite_flash, 2);
	rb_define_method(cSprite, "update", sprite_update, 0);
	rb_define_method(cSprite, "width", sprite_width, 0);
	rb_define_method(cSprite, "height", sprite_height, 0);
	DEF_ATTR(cSprite, "bitmap", sprite_bitmap, sprite_set_bitmap);
	DEF_ATTR(cSprite, "src_rect", sprite_src_rect, sprite_set_src_rect);
	DEF_ATTR(cSprite, "color", sprite_color, sprite_set_color);
	DEF_ATTR(cSprite, "tone", sprite_tone, sprite_set_tone);
	DEF_ATTR(cSprite, "viewport", sprite_viewport, sprite_set_viewport);
	DEF_ATTR(cSprite, "z", sprite_z, sprite_set_z);
	DEF_ATTR(cSprite, "x", sprite_get_x, sprite_set_x);
	DEF_ATTR(cSprite, "y", sprite_get_y, sprite_set_y);
	DEF_ATTR(cSprite, "ox", sprite_get_ox, sprite_set_ox);
	DEF_ATTR(cSprite, "oy", sprite_get_oy, sprite_set_oy);
	DEF_ATTR(cSprite, "opacity", sprite_get_opacity, sprite_set_opacity);
	DEF_ATTR(cSprite, "blend_type", sprite_get_blend_type, sprite_set_blend_type);
	DEF_ATTR(cSprite, "bush_depth", sprite_get_bush_depth, sprite_set_bush_depth);
	DEF_ATTR(cSprite, "bush_opacity", sprite_get_bush_opacity, sprite_set_bush_opacity);
	DEF_ATTR(cSprite, "zoom_x", sprite_get_zoom_x, sprite_set_zoom_x);
	DEF_ATTR(cSprite, "zoom_y", sprite_get_zoom_y, sprite_set_zoom_y);
	DEF_ATTR(cSprite, "angle", sprite_get_angle, sprite_set_angle);
	DEF_ATTR(cSprite, "visible", sprite_get_visible, sprite_set_visible);
	DEF_ATTR(cSprite, "mirror", sprite_get_mirror, sprite_set_mirror);
	DEF_ATTR(cSprite, "wave_amp", sprite_get_wave_amp, sprite_set_wave_amp);
	DEF_ATTR(cSprite, "wave_length", sprite_get_wave_length, sprite_set_wave_length);
	DEF_ATTR(cSprite, "wave_speed", sprite_get_wave_speed, sprite_set_wave_speed);
	DEF_ATTR(cSprite, "wave_phase", sprite_get_wave_phase, sprite_set_wave_phase);

	cPlane = rb_define_class("Plane", rb_cObject);
	rb_define_alloc_func(cPlane, plane_alloc);
	rb_define_method(cPlane, "initialize", plane_init, -1);
	rb_define_method(cPlane, "dispose", plane_dispose, 0);
	rb_define_method(cPlane, "disposed?", plane_disposed, 0);
	DEF_ATTR(cPlane, "bitmap", plane_bitmap, plane_set_bitmap);
	DEF_ATTR(cPlane, "color", plane_color, plane_set_color);
	DEF_ATTR(cPlane, "tone", plane_tone, plane_set_tone);
	DEF_ATTR(cPlane, "viewport", plane_viewport, plane_set_viewport);
	DEF_ATTR(cPlane, "z", plane_z, plane_set_z);
	DEF_ATTR(cPlane, "ox", plane_get_ox, plane_set_ox);
	DEF_ATTR(cPlane, "oy", plane_get_oy, plane_set_oy);
	DEF_ATTR(cPlane, "opacity", plane_get_opacity, plane_set_opacity);
	DEF_ATTR(cPlane, "blend_type", plane_get_blend_type, plane_set_blend_type);
	DEF_ATTR(cPlane, "zoom_x", plane_zoom_x, plane_set_zoom_x);
	DEF_ATTR(cPlane, "zoom_y", plane_zoom_y, plane_set_zoom_y);
	DEF_ATTR(cPlane, "visible", plane_visible, plane_set_visible);
}
