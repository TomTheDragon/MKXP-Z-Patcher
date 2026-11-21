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
	if (!bitmap_px(src))
		return;
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

/* the reset (SELECT, like F12): every sprite, window, plane, tilemap and
 * viewport still there is disposed, as in mkxp-z (the game's objects are
 * simply dropped; until the garbage collector freed them they stayed on the
 * screen, e.g. a message box in the middle of its text) */
static VALUE dispose_all(VALUE self)
{
	(void) self;
	VALUE all = rb_ary_new();
	for (Element *e = elements; e; e = e->next)
		if (!RB_SPECIAL_CONST_P(e->self) && !RTEST(rb_ary_includes(all, e->self)))
			rb_ary_push(all, e->self);
	/* the engine's own dispose (Window#dispose etc.), not the game's: its
	 * overrides also dispose other windows, which may be gone already
	 * (Changed: "disposed window" on a reset), as mkxp-z does */
	VALUE bases[] = { cWindow, cSprite, cPlane, cTilemap, cViewport };
	ID id_im = rb_intern("instance_method"), id_bind_call = rb_intern("bind_call");
	VALUE sym_dispose = ID2SYM(rb_intern("dispose")), sym_disposed = ID2SYM(rb_intern("disposed?"));
	for (long i = 0; i < RARRAY_LEN(all); i++) {
		VALUE o = RARRAY_AREF(all, i);
		for (size_t k = 0; k < sizeof bases / sizeof *bases; k++) {
			if (!bases[k] || !RTEST(rb_obj_is_kind_of(o, bases[k])))
				continue;
			VALUE gone = rb_funcall(rb_funcall(bases[k], id_im, 1, sym_disposed), id_bind_call, 1, o);
			if (!RTEST(gone))
				rb_funcall(rb_funcall(bases[k], id_im, 1, sym_dispose), id_bind_call, 1, o);
			break;
		}
	}
	return Qnil;
}

static int by_z(const void *a, const void *b)
{
	const Element *x = *(Element *const *) a, *y = *(Element *const *) b;
	if (x->z != y->z)
		return x->z < y->z ? -1 : 1;
	/* same z: by y (sprites; everything else counts as 0), as RGSS2/3 and
	 * mkxp-z do: characters lower on the screen in front */
	int ya = x->sort_y ? *x->sort_y : 0, yb = y->sort_y ? *y->sort_y : 0;
	if (ya != yb)
		return ya < yb ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

/* Ruby (gc.c): true for an object the garbage collector found dead but
 * hasn't freed yet (lazy sweeping; our C objects are even freed later, in
 * a postponed job). Such a sprite stays in the element list until then,
 * while its bitmap may already be freed: drawing it wrote the texture
 * bookkeeping into freed memory (crashes on the console after a while). */
int rb_objspace_garbage_object_p(VALUE obj);

static bool element_alive(const Element *e)
{
	return RB_SPECIAL_CONST_P(e->self) || !rb_objspace_garbage_object_p(e->self);
}

static Element **collect(VALUE viewport, int *n)
{
	int count = 0;
	for (Element *e = elements; e; e = e->next)
		if (e->viewport == viewport && element_alive(e))
			count++;
	Element **list = malloc(sizeof *list * (count ? count : 1));
	int i = 0;
	for (Element *e = elements; e; e = e->next)
		if (e->viewport == viewport && element_alive(e) && i < count)
			list[i++] = e;
	count = i;
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
	s->el.sort_y = &s->y;
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
static VALUE sprite_set_wide(VALUE self, VALUE v) { sprite_get(self)->el.wide = RTEST(v); return v; }
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
	if (c->gpu && gpu_plane(c, b, bx, by, s->zoom_x, s->zoom_y, s->opacity, s->blend_type,
	                        color_get(s->color), tone_get(s->tone)))
		return;
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
	/* elements marked mkxpz_wide (message boxes) may use the sides */
	Canvas wc = *c;
	if (c->gpu && gpu_margin > 0)
		wc.clip = (Rect) { -gpu_margin, 0, c->w + 2 * gpu_margin, c->h };
	for (int i = 0; i < n; i++)
		list[i]->draw(list[i], list[i]->wide ? &wc : c, 0, 0);
	free(list);
}

void init_scene(void)
{
	rb_define_global_function("mkxpz_dispose_all", dispose_all, 0);
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
	rb_define_method(cSprite, "mkxpz_wide=", sprite_set_wide, 1);
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

/* --- Faster standard VX Ace scripts: Sprite_Character#update in C.
 * Each character sprite on a map runs this every frame, and in Ruby on the
 * original 3DS it costs about 90 us per sprite (10 ms on a map with 100
 * events). boot.rb installs this only when the standard methods it stands
 * for are unchanged (checked by their source text), and asks again for every
 * class it meets (mkxpz_fast_ok?): a game that changes any of them, in a
 * subclass too, gets the original Ruby (mkxpz_slow_update). Uncommon things
 * (animations, balloons, a new graphic, odd values) call the original Ruby
 * methods. */
static ID id_slow_update, id_fast_ok, id_update_animation, id_update_bitmap, id_update_src_rect,
	id_update_position, id_update_other, id_update_balloon, id_setup_new_effect, id_clear,
	iv_character, iv_animation, iv_tile_id, iv_character_name, iv_character_index, iv_cw, iv_ch,
	iv_balloon_duration, iv_real_x, iv_real_y, iv_priority_type, iv_pattern, iv_direction, iv_opacity,
	iv_blend_type, iv_bush_depth, iv_transparent, iv_animation_id, iv_balloon_id, iv_jump_peak,
	iv_jump_count, iv_display_x, iv_display_y, iv_map, iv_width, iv_height, iv_scroll_type,
	cv_ani_checker, cv_ani_spr_checker;
static VALUE cSpriteBase;
static ID id_game_map;

static struct { VALUE klass; int kind; bool ok; } fast_cache[48];
static int fast_n;

static bool fast_ok(VALUE klass, int kind)
{
	static const char *const kinds[] = { "sprite", "character", "map", "event_update" };
	if (FL_TEST(klass, FL_SINGLETON))
		return false;
	for (int i = 0; i < fast_n; i++)
		if (fast_cache[i].klass == klass && fast_cache[i].kind == kind)
			return fast_cache[i].ok;
	bool ok = RTEST(rb_funcall(Qnil, id_fast_ok, 2, klass, ID2SYM(rb_intern(kinds[kind]))));
	if (fast_n < (int) (sizeof fast_cache / sizeof *fast_cache)) {
		rb_gc_register_mark_object(klass);
		fast_cache[fast_n].klass = klass;
		fast_cache[fast_n].kind = kind;
		fast_cache[fast_n++].ok = ok;
	}
	return ok;
}

/* a Ruby number: Integer (fixnum) or Float, with Ruby's arithmetic */
typedef struct { bool f; long long i; double d; } Num;
static bool num_of(VALUE v, Num *n)
{
	if (FIXNUM_P(v)) { *n = (Num) { false, FIX2LONG(v), 0 }; return true; }
	if (RB_FLOAT_TYPE_P(v)) { *n = (Num) { true, 0, RFLOAT_VALUE(v) }; return true; }
	return false;
}
static double num_d(Num a) { return a.f ? a.d : (double) a.i; }
static Num num_sub(Num a, Num b) { return a.f || b.f ? (Num) { true, 0, num_d(a) - num_d(b) } : (Num) { false, a.i - b.i, 0 }; }
static Num num_addi(Num a, long long b) { return a.f ? (Num) { true, 0, a.d + (double) b } : (Num) { false, a.i + b, 0 }; }
static Num num_muli(Num a, long long b) { return a.f ? (Num) { true, 0, a.d * (double) b } : (Num) { false, a.i * b, 0 }; }
static bool num_lt(Num a, Num b) { return a.f || b.f ? num_d(a) < num_d(b) : a.i < b.i; }
/* what the Sprite setters make of it (Float: truncated) */
static int num_int(Num a, int lo, int hi)
{
	double v = a.f ? a.d : (double) a.i;
	if (v != v)
		return 0;
	return v < lo ? lo : v > hi ? hi : (int) v;
}
static long long floor_div(long long a, long long b) { long long q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }
static long long floor_mod(long long a, long long b) { return a - floor_div(a, b) * b; }

/* Game_Map#adjust_x / adjust_y */

static Num adjust(Num v, Num disp, bool loop, long long size, long long tiles)
{
	if (loop && num_lt(v, num_sub(disp, (Num) { false, floor_div(size - tiles, 2), 0 })))
		return num_addi(num_sub(v, disp), size);
	return num_sub(v, disp);
}

/* update_position; false: values it doesn't handle */
static bool fast_position(Sprite *s, VALUE chr, VALUE gm)
{
	VALUE map = IV(gm, iv_map);
	if (!RB_TYPE_P(map, T_OBJECT))
		return false;
	VALUE mw = IV(map, iv_width), mh = IV(map, iv_height), st = IV(map, iv_scroll_type);
	VALUE tid = IV(chr, iv_tile_id), name = IV(chr, iv_character_name);
	VALUE peak = IV(chr, iv_jump_peak), count = IV(chr, iv_jump_count);
	VALUE prio = IV(chr, iv_priority_type);
	Num rx, ry, dx, dy;
	if (!FIXNUM_P(mw) || !FIXNUM_P(mh) || !FIXNUM_P(st) || !FIXNUM_P(tid) || !RB_TYPE_P(name, T_STRING) ||
	    !FIXNUM_P(peak) || !FIXNUM_P(count) || !FIXNUM_P(prio) || !num_of(IV(chr, iv_real_x), &rx) ||
	    !num_of(IV(chr, iv_real_y), &ry) || !num_of(IV(gm, iv_display_x), &dx) ||
	    !num_of(IV(gm, iv_display_y), &dy))
		return false;
	long scroll = FIX2LONG(st);
	Num ax = adjust(rx, dx, scroll == 2 || scroll == 3, FIX2LONG(mw), screen_w / 32);
	Num ay = adjust(ry, dy, scroll == 1 || scroll == 3, FIX2LONG(mh), screen_h / 32);
	/* shift_y: object_character? ? 0 : 4 */
	bool object = FIX2LONG(tid) > 0 || (RSTRING_LEN(name) > 0 && RSTRING_PTR(name)[0] == '!');
	long long p = FIX2LONG(peak), c = FIX2LONG(count), d = c - p < 0 ? p - c : c - p;
	long long jump = floor_div(p * p - d * d, 2);
	Num sx = num_addi(num_muli(ax, 32), 16);
	Num sy = num_addi(num_addi(num_addi(num_muli(ay, 32), 32), -(object ? 0 : 4)), -jump);
	s->x = num_int(sx, -999999, 999999);
	s->y = num_int(sy, -999999, 999999);
	s->el.z = (int) (FIX2LONG(prio) * 100);
	return true;
}

/* @@ani_checker.clear and @@_ani_spr_checker.clear (the class variables
 * are looked up once a frame) */
static VALUE checkers[2];
static int checkers_frame = -1;
static void clear_checker(int which)
{
	if (checkers_frame != graphics_frame()) {
		checkers[0] = rb_cvar_get(cSpriteBase, cv_ani_checker);
		checkers[1] = rb_cvar_get(cSpriteBase, cv_ani_spr_checker);
		checkers_frame = graphics_frame();
	}
	VALUE a = checkers[which];
	if (RB_TYPE_P(a, T_ARRAY)) {
		if (RARRAY_LEN(a))
			rb_ary_clear(a);
	} else {
		rb_funcall(a, id_clear, 0);
	}
}

static VALUE fast_update(VALUE self)
{
	VALUE chr = IV(self, iv_character);
	VALUE gm = mkxpz_gvar(id_game_map);
	if (!fast_ok(CLASS_OF(self), 0) || !fast_ok(CLASS_OF(chr), 1) || !fast_ok(CLASS_OF(gm), 2))
		return rb_funcall(self, id_slow_update, 0);
	/* Sprite_Base#update: Sprite#update, update_animation, the checkers */
	sprite_update(self);
	if (!NIL_P(IV(self, iv_animation)))
		rb_funcall(self, id_update_animation, 0);
	clear_checker(0);
	clear_checker(1);
	/* update_bitmap: only does something when graphic_changed? */
	VALUE my_tile = IV(self, iv_tile_id);
	if (!rb_equal(my_tile, IV(chr, iv_tile_id)) ||
	    !rb_equal(IV(self, iv_character_name), IV(chr, iv_character_name)) ||
	    !rb_equal(IV(self, iv_character_index), IV(chr, iv_character_index)))
		rb_funcall(self, id_update_bitmap, 0);
	/* update_src_rect */
	if (rb_equal(IV(self, iv_tile_id), INT2FIX(0))) { /* (again: update_bitmap may change it) */
		VALUE idx = IV(chr, iv_character_index), pat = IV(chr, iv_pattern);
		VALUE dir = IV(chr, iv_direction), cw = IV(self, iv_cw), ch = IV(self, iv_ch);
		if (FIXNUM_P(idx) && FIXNUM_P(pat) && FIXNUM_P(dir) && FIXNUM_P(cw) && FIXNUM_P(ch)) {
			long long index = FIX2LONG(idx), pattern = FIX2LONG(pat) < 3 ? FIX2LONG(pat) : 1;
			Rect *r = rect_get(sprite_get(self)->src_rect);
			r->x = (int) ((floor_mod(index, 4) * 3 + pattern) * FIX2LONG(cw));
			r->y = (int) ((floor_div(index, 4) * 4 + floor_div(FIX2LONG(dir) - 2, 2)) * FIX2LONG(ch));
			r->w = (int) FIX2LONG(cw);
			r->h = (int) FIX2LONG(ch);
		} else {
			rb_funcall(self, id_update_src_rect, 0);
		}
	}
	/* update_position */
	if (!fast_position(sprite_get(self), chr, gm))
		rb_funcall(self, id_update_position, 0);
	/* update_other */
	VALUE op = IV(chr, iv_opacity), bl = IV(chr, iv_blend_type), bu = IV(chr, iv_bush_depth);
	if (FIXNUM_P(op) && FIXNUM_P(bl) && FIXNUM_P(bu)) {
		Sprite *s = sprite_get(self);
		s->opacity = CLAMP((int) FIX2LONG(op), 0, 255);
		s->blend_type = CLAMP((int) FIX2LONG(bl), 0, 2);
		s->bush_depth = CLAMP((int) FIX2LONG(bu), 0, 999999);
		s->visible = !RTEST(IV(chr, iv_transparent));
	} else {
		rb_funcall(self, id_update_other, 0);
	}
	/* update_balloon: only while one shows */
	VALUE bd = IV(self, iv_balloon_duration);
	if (!FIXNUM_P(bd) || FIX2LONG(bd) > 0)
		rb_funcall(self, id_update_balloon, 0);
	/* setup_new_effect: only with an animation or balloon to start */
	VALUE aid = IV(chr, iv_animation_id), bid = IV(chr, iv_balloon_id);
	if (!FIXNUM_P(aid) || FIX2LONG(aid) > 0 || !FIXNUM_P(bid) || FIX2LONG(bid) > 0)
		rb_funcall(self, id_setup_new_effect, 0);
	return Qnil;
}

/* --- the same for RPG Maker VX (RGSS2): Sprite_Character#update with
 * Sprite_Base#update, update_bitmap, update_src_rect and Game_Character's
 * screen_x / screen_y / screen_z (positions * 256, Game_Map#adjust_x with
 * @margin_x). An animation playing or to start, or a balloon to start: the
 * whole Ruby update (mkxpz_slow_update). Changed has 269 events on a map,
 * each one's sprite updated every frame. */
static ID iv_margin_x, iv_margin_y, cv_animations, id_screen_x, id_screen_y, id_screen_z;
static VALUE animations_cv;
static int animations_frame = -1;

/* Game_Map#adjust_x / adjust_y (VX); false: not integers */
static bool adjust_vx(VALUE v, VALUE disp, VALUE margin, bool loop, VALUE size, long long *out)
{
	if (!FIXNUM_P(v) || !FIXNUM_P(disp) || !FIXNUM_P(margin) || !FIXNUM_P(size))
		return false;
	long long x = FIX2LONG(v), d = FIX2LONG(disp);
	*out = loop && x < d - FIX2LONG(margin) ? x - d + FIX2LONG(size) * 256 : x - d;
	return true;
}

static VALUE fast_update_vx(VALUE self)
{
	VALUE chr = IV(self, iv_character);
	VALUE gm = mkxpz_gvar(id_game_map);
	if (!fast_ok(CLASS_OF(self), 0) || !fast_ok(CLASS_OF(chr), 1) || !fast_ok(CLASS_OF(gm), 2))
		return rb_funcall(self, id_slow_update, 0);
	VALUE aid = IV(chr, iv_animation_id), bid = IV(chr, iv_balloon_id);
	if (!NIL_P(IV(self, iv_animation)) || !FIXNUM_P(aid) || FIX2LONG(aid) != 0 || !FIXNUM_P(bid) ||
	    FIX2LONG(bid) != 0)
		return rb_funcall(self, id_slow_update, 0);
	/* Sprite_Base#update: Sprite#update, @@animations.clear */
	sprite_update(self);
	if (animations_frame != graphics_frame()) {
		animations_cv = rb_cvar_get(cSpriteBase, cv_animations);
		animations_frame = graphics_frame();
	}
	if (RB_TYPE_P(animations_cv, T_ARRAY)) {
		if (RARRAY_LEN(animations_cv))
			rb_ary_clear(animations_cv);
	} else {
		rb_funcall(animations_cv, id_clear, 0);
	}
	/* update_bitmap: only when the graphic changed */
	if (!rb_equal(IV(self, iv_tile_id), IV(chr, iv_tile_id)) ||
	    !rb_equal(IV(self, iv_character_name), IV(chr, iv_character_name)) ||
	    !rb_equal(IV(self, iv_character_index), IV(chr, iv_character_index)))
		rb_funcall(self, id_update_bitmap, 0);
	Sprite *s = sprite_get(self);
	s->visible = !RTEST(IV(chr, iv_transparent));
	/* update_src_rect */
	VALUE tile = IV(self, iv_tile_id);
	if (rb_equal(tile, INT2FIX(0))) {
		VALUE idx = IV(chr, iv_character_index), pat = IV(chr, iv_pattern);
		VALUE dir = IV(chr, iv_direction), cw = IV(self, iv_cw), ch = IV(self, iv_ch);
		if (FIXNUM_P(idx) && FIXNUM_P(pat) && FIXNUM_P(dir) && FIXNUM_P(cw) && FIXNUM_P(ch)) {
			long long index = FIX2LONG(idx), pattern = FIX2LONG(pat) < 3 ? FIX2LONG(pat) : 1;
			Rect *r = rect_get(s->src_rect);
			r->x = (int) ((floor_mod(index, 4) * 3 + pattern) * FIX2LONG(cw));
			r->y = (int) ((floor_div(index, 4) * 4 + floor_div(FIX2LONG(dir) - 2, 2)) * FIX2LONG(ch));
			r->w = (int) FIX2LONG(cw);
			r->h = (int) FIX2LONG(ch);
		} else {
			rb_funcall(self, id_update_src_rect, 0);
		}
	}
	/* screen_x, screen_y, screen_z */
	VALUE map = IV(gm, iv_map);
	VALUE ctile = IV(chr, iv_tile_id), name = IV(chr, iv_character_name), prio = IV(chr, iv_priority_type);
	VALUE peak = IV(chr, iv_jump_peak), count = IV(chr, iv_jump_count);
	long long ax, ay;
	VALUE st = RB_TYPE_P(map, T_OBJECT) ? IV(map, iv_scroll_type) : Qnil;
	if (FIXNUM_P(st) && FIXNUM_P(ctile) && RB_TYPE_P(name, T_STRING) && FIXNUM_P(peak) && FIXNUM_P(count) &&
	    adjust_vx(IV(chr, iv_real_x), IV(gm, iv_display_x), IV(gm, iv_margin_x),
	              FIX2LONG(st) == 2 || FIX2LONG(st) == 3, IV(map, iv_width), &ax) &&
	    adjust_vx(IV(chr, iv_real_y), IV(gm, iv_display_y), IV(gm, iv_margin_y),
	              FIX2LONG(st) == 1 || FIX2LONG(st) == 3, IV(map, iv_height), &ay)) {
		bool object = FIX2LONG(ctile) > 0 || (RSTRING_LEN(name) > 0 && RSTRING_PTR(name)[0] == '!');
		long long p = FIX2LONG(peak), c = FIX2LONG(count), n = c >= p ? c - p : p - c;
		s->x = (int) CLAMP(floor_div(ax + 8007, 8) - 1000 + 16, -999999, 999999);
		long long y = floor_div(ay + 8007, 8) - 1000 + 32 - (object ? 0 : 4);
		s->y = (int) CLAMP(y - floor_div(p * p - n * n, 2), -999999, 999999);
	} else {
		VALUE x = rb_funcall(chr, id_screen_x, 0), y = rb_funcall(chr, id_screen_y, 0);
		rb_funcall(self, rb_intern("x="), 1, x);
		rb_funcall(self, rb_intern("y="), 1, y);
	}
	/* screen_z: 200 above, 60 below, a tile by its passage flags (Ruby), 100 */
	if (FIXNUM_P(prio) && FIX2LONG(prio) == 2)
		s->el.z = 200;
	else if (FIXNUM_P(prio) && FIX2LONG(prio) == 0)
		s->el.z = 60;
	else if (FIXNUM_P(prio) && FIXNUM_P(ctile) && FIX2LONG(ctile) <= 0)
		s->el.z = 100;
	else
		rb_funcall(self, rb_intern("z="), 1, rb_funcall(chr, id_screen_z, 0)); /* (a tile: passages) */
	/* opacity, blend_type, bush_depth */
	VALUE op = IV(chr, iv_opacity), bl = IV(chr, iv_blend_type), bu = IV(chr, iv_bush_depth);
	if (FIXNUM_P(op) && FIXNUM_P(bl) && FIXNUM_P(bu)) {
		s->opacity = CLAMP((int) FIX2LONG(op), 0, 255);
		s->blend_type = CLAMP((int) FIX2LONG(bl), 0, 2);
		s->bush_depth = CLAMP((int) FIX2LONG(bu), 0, 999999);
	} else {
		rb_funcall(self, rb_intern("opacity="), 1, op);
		rb_funcall(self, rb_intern("blend_type="), 1, bl);
		rb_funcall(self, rb_intern("bush_depth="), 1, bu);
	}
	/* update_balloon: only while one shows */
	VALUE bd = IV(self, iv_balloon_duration);
	if (!FIXNUM_P(bd) || FIX2LONG(bd) > 0)
		rb_funcall(self, id_update_balloon, 0);
	return Qnil;
}

static VALUE fast_install_vx(VALUE self, VALUE klass)
{
	(void) self;
	cSpriteBase = rb_class_superclass(klass);
	rb_gc_register_mark_object(cSpriteBase);
	rb_gc_register_address(&animations_cv);
	rb_define_method(klass, "update", fast_update_vx, 0);
	return Qtrue;
}

/* --- Game_Event#update for an event standing still: the standard methods
 * in C (Game_CharacterBase#update, update_animation, update_anime_count,
 * the three update_stop, update_self_movement, near_the_screen?,
 * check_event_trigger_auto). Moving or jumping events, and any value of
 * another type than expected, take the original Ruby update (checked
 * before anything changes); a step (move_type_*), update_anime_pattern,
 * update_routine_move, start and the event's own script are called in
 * Ruby. On the console these were ~15 Ruby calls per event per frame. */
static ID id_slow_event_update, id_update_anime_pattern, id_update_routine_move, id_move_type[4], id_start,
	id_update_interpreter, iv_x, iv_y, iv_original_pattern, iv_move_speed, iv_anime_count, iv_step_anime,
	iv_locked, iv_stop_count, iv_move_route_forcing, iv_move_type, iv_move_frequency, iv_trigger, iv_interpreter;

static bool num_eq(Num a, Num b) { return a.f || b.f ? num_d(a) == num_d(b) : a.i == b.i; }
static VALUE num_value(Num a) { return a.f ? DBL2NUM(a.d) : LL2NUM(a.i); }

/* near_the_screen?(12, 8) */
static bool near_the_screen(VALUE self, VALUE gm, bool *ok)
{
	*ok = false;
	VALUE map = IV(gm, iv_map);
	if (!RB_TYPE_P(map, T_OBJECT))
		return false;
	VALUE mw = IV(map, iv_width), mh = IV(map, iv_height), st = IV(map, iv_scroll_type);
	Num rx, ry, dx, dy;
	if (!FIXNUM_P(mw) || !FIXNUM_P(mh) || !FIXNUM_P(st) || !num_of(IV(self, iv_real_x), &rx) ||
	    !num_of(IV(self, iv_real_y), &ry) || !num_of(IV(gm, iv_display_x), &dx) || !num_of(IV(gm, iv_display_y), &dy))
		return false;
	*ok = true;
	long scroll = FIX2LONG(st);
	Num ax = num_addi(adjust(rx, dx, scroll == 2 || scroll == 3, FIX2LONG(mw), screen_w / 32), -(screen_w / 2 / 32));
	Num ay = num_addi(adjust(ry, dy, scroll == 1 || scroll == 3, FIX2LONG(mh), screen_h / 32), -(screen_h / 2 / 32));
	Num lo_x = { false, -12, 0 }, hi_x = { false, 12, 0 }, lo_y = { false, -8, 0 }, hi_y = { false, 8, 0 };
	return !num_lt(ax, lo_x) && !num_lt(hi_x, ax) && !num_lt(ay, lo_y) && !num_lt(hi_y, ay);
}

static VALUE fast_event_update(VALUE self)
{
	VALUE gm = mkxpz_gvar(id_game_map);
	if (!fast_ok(CLASS_OF(self), 3) || RB_SPECIAL_CONST_P(gm) || !fast_ok(CLASS_OF(gm), 2))
		return rb_funcall(self, id_slow_event_update, 0);
	/* everything checked first: only an event that stands still, with
	 * values of the usual types */
	VALUE jc = IV(self, iv_jump_count), pat = IV(self, iv_pattern), opat = IV(self, iv_original_pattern);
	VALUE ms = IV(self, iv_move_speed), sc = IV(self, iv_stop_count), mt = IV(self, iv_move_type);
	VALUE mf = IV(self, iv_move_frequency);
	Num rx, ry, x, y, ac;
	if (!FIXNUM_P(jc) || FIX2LONG(jc) > 0 || !num_of(IV(self, iv_real_x), &rx) || !num_of(IV(self, iv_real_y), &ry) ||
	    !num_of(IV(self, iv_x), &x) || !num_of(IV(self, iv_y), &y) || !num_eq(rx, x) || !num_eq(ry, y) ||
	    !FIXNUM_P(pat) || !FIXNUM_P(opat) || !FIXNUM_P(ms) || !FIXNUM_P(sc) || !FIXNUM_P(mt) || !FIXNUM_P(mf) ||
	    !num_of(IV(self, iv_anime_count), &ac))
		return rb_funcall(self, id_slow_event_update, 0);
	/* update_animation: update_anime_count (not moving) */
	if (RTEST(IV(self, iv_step_anime)) || pat != opat) {
		ac = num_addi(ac, 1);
		rb_ivar_set(self, iv_anime_count, num_value(ac));
	}
	/* if @anime_count > 18 - real_move_speed * 2 (dash? is false) */
	if (num_lt((Num) { false, 18 - FIX2LONG(ms) * 2, 0 }, ac)) {
		rb_funcall(self, id_update_anime_pattern, 0);
		rb_ivar_set(self, iv_anime_count, INT2FIX(0));
	}
	/* update_stop: Game_CharacterBase */
	if (!RTEST(IV(self, iv_locked))) {
		sc = IV(self, iv_stop_count);
		rb_ivar_set(self, iv_stop_count, FIXNUM_P(sc) ? LONG2NUM(FIX2LONG(sc) + 1) : rb_funcall(sc, rb_intern("+"), 1, INT2FIX(1)));
	}
	/* Game_Character */
	if (RTEST(IV(self, iv_move_route_forcing)))
		rb_funcall(self, id_update_routine_move, 0);
	/* Game_Event: update_self_movement unless @move_route_forcing */
	if (!RTEST(IV(self, iv_move_route_forcing))) {
		bool ok;
		bool near = near_the_screen(self, gm, &ok);
		sc = IV(self, iv_stop_count);
		mf = IV(self, iv_move_frequency);
		mt = IV(self, iv_move_type);
		if (!ok || !FIXNUM_P(sc) || !FIXNUM_P(mf) || !FIXNUM_P(mt)) {
			rb_funcall(self, rb_intern("update_self_movement"), 0);
		} else if (near && FIX2LONG(sc) > 30 * (5 - FIX2LONG(mf))) {
			long t = FIX2LONG(mt);
			if (t >= 1 && t <= 3)
				rb_funcall(self, id_move_type[t], 0);
		}
	}
	/* check_event_trigger_auto: start if @trigger == 3 */
	VALUE tr = IV(self, iv_trigger);
	if (FIXNUM_P(tr) ? tr == INT2FIX(3) : rb_equal(tr, INT2FIX(3)))
		rb_funcall(self, id_start, 0);
	/* the event's own script (parallel) */
	if (RTEST(IV(self, iv_interpreter)))
		return rb_funcall(self, id_update_interpreter, 0);
	return Qnil;
}

static VALUE fast_install_event_update(VALUE self, VALUE klass)
{
	(void) self;
	rb_define_method(klass, "update", fast_event_update, 0);
	return Qtrue;
}

/* (boot.rb, once it checked the scripts) */
static VALUE fast_install(VALUE self, VALUE klass)
{
	(void) self;
	cSpriteBase = rb_class_superclass(klass);
	rb_gc_register_mark_object(cSpriteBase);
	rb_define_method(klass, "update", fast_update, 0);
	return Qtrue;
}

void init_fast(void)
{
#define I(v, s) v = rb_intern(s)
	I(id_game_map, "$game_map");
	rb_gc_register_address(&checkers[0]);
	rb_gc_register_address(&checkers[1]);
	I(id_slow_update, "mkxpz_slow_update"); I(id_fast_ok, "mkxpz_fast_ok?");
	I(id_update_animation, "update_animation"); I(id_update_bitmap, "update_bitmap");
	I(id_update_src_rect, "update_src_rect"); I(id_update_position, "update_position");
	I(id_update_other, "update_other"); I(id_update_balloon, "update_balloon");
	I(id_setup_new_effect, "setup_new_effect"); I(id_clear, "clear");
	I(iv_character, "@character"); I(iv_animation, "@animation"); I(iv_tile_id, "@tile_id");
	I(iv_character_name, "@character_name"); I(iv_character_index, "@character_index");
	I(iv_cw, "@cw"); I(iv_ch, "@ch"); I(iv_balloon_duration, "@balloon_duration");
	I(iv_real_x, "@real_x"); I(iv_real_y, "@real_y"); I(iv_priority_type, "@priority_type");
	I(iv_pattern, "@pattern"); I(iv_direction, "@direction"); I(iv_opacity, "@opacity");
	I(iv_blend_type, "@blend_type"); I(iv_bush_depth, "@bush_depth"); I(iv_transparent, "@transparent");
	I(iv_animation_id, "@animation_id"); I(iv_balloon_id, "@balloon_id"); I(iv_jump_peak, "@jump_peak");
	I(iv_jump_count, "@jump_count"); I(iv_display_x, "@display_x"); I(iv_display_y, "@display_y");
	I(iv_map, "@map"); I(iv_width, "@width"); I(iv_height, "@height"); I(iv_scroll_type, "@scroll_type");
	I(cv_ani_checker, "@@ani_checker"); I(cv_ani_spr_checker, "@@ani_spr_checker");
	I(iv_margin_x, "@margin_x"); I(iv_margin_y, "@margin_y"); I(cv_animations, "@@animations");
	I(id_screen_x, "screen_x"); I(id_screen_y, "screen_y"); I(id_screen_z, "screen_z");
	I(id_slow_event_update, "mkxpz_slow_event_update"); I(id_update_anime_pattern, "update_anime_pattern");
	I(id_update_routine_move, "update_routine_move"); I(id_move_type[1], "move_type_random");
	I(id_move_type[2], "move_type_toward_player"); I(id_move_type[3], "move_type_custom"); I(id_start, "start");
	I(id_update_interpreter, "mkxpz_update_interpreter"); I(iv_x, "@x"); I(iv_y, "@y");
	I(iv_original_pattern, "@original_pattern"); I(iv_move_speed, "@move_speed"); I(iv_anime_count, "@anime_count");
	I(iv_step_anime, "@step_anime"); I(iv_locked, "@locked"); I(iv_stop_count, "@stop_count");
	I(iv_move_route_forcing, "@move_route_forcing"); I(iv_move_type, "@move_type");
	I(iv_move_frequency, "@move_frequency"); I(iv_trigger, "@trigger"); I(iv_interpreter, "@interpreter");
#undef I
	rb_define_global_function("mkxpz_fast_install", fast_install, 1);
	rb_define_global_function("mkxpz_fast_install_vx", fast_install_vx, 1);
	rb_define_global_function("mkxpz_fast_install_event_update", fast_install_event_update, 1);
}
