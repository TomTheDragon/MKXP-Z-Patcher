/* Window (VX / VX Ace windowskins: 128x128). */
#include "engine.h"
#include <stdlib.h>

VALUE cWindow;

typedef struct {
	Element el;
	VALUE windowskin, contents, cursor_rect, tone;
	bool visible, active, pause, arrows_visible, disposed;
	int x, y, w, h, ox, oy, padding, padding_bottom;
	int opacity, back_opacity, contents_opacity, openness;
	int frame; /* counts updates: cursor blink, pause sign */
} Window;

static void window_draw(Element *e, Canvas *c, int ox, int oy);

static void window_mark(void *p)
{
	Window *w = p;
	rb_gc_mark(w->windowskin);
	rb_gc_mark(w->contents);
	rb_gc_mark(w->cursor_rect);
	rb_gc_mark(w->tone);
	rb_gc_mark(w->el.viewport);
}
static void window_free(void *p)
{
	Window *w = p;
	if (!w->disposed)
		element_unregister(&w->el);
	xfree(w);
}
static const rb_data_type_t window_type = { "Window", { window_mark, window_free, NULL }, 0, 0, 0 };
static VALUE window_alloc(VALUE klass)
{
	Window *w;
	VALUE self = TypedData_Make_Struct(klass, Window, &window_type, w);
	w->windowskin = w->contents = w->cursor_rect = w->tone = Qnil;
	w->el.viewport = Qnil;
	return self;
}
static Window *window_get(VALUE self)
{
	Window *w = rb_check_typeddata(self, &window_type);
	if (w->disposed)
		rb_raise(eRGSSError, "disposed window");
	return w;
}

/* message boxes: may use the screen's sides (gpu_margin) */
static VALUE window_set_wide(VALUE self, VALUE v) { window_get(self)->el.wide = RTEST(v); return v; }

/* Window.new, Window.new(viewport) or (VX Ace) Window.new(x, y, w, h) */
static VALUE window_init(int argc, VALUE *argv, VALUE self)
{
	Window *w = rb_check_typeddata(self, &window_type);
	VALUE vp = Qnil;
	if (argc == 4) {
		w->x = num2int(argv[0]); w->y = num2int(argv[1]);
		w->w = num2int(argv[2]); w->h = num2int(argv[3]);
	} else if (argc == 1) {
		vp = argv[0];
	}
	w->contents = bitmap_new_size(1, 1);
	w->cursor_rect = rect_new(0, 0, 0, 0);
	w->tone = tone_new(0, 0, 0, 0);
	w->visible = w->active = w->arrows_visible = true;
	w->padding = rgss_version >= 3 ? 12 : 16;
	w->padding_bottom = w->padding;
	w->opacity = 255;
	w->back_opacity = rgss_version >= 3 ? 192 : 200;
	w->contents_opacity = 255;
	w->openness = 255;
	w->el.z = rgss_version >= 3 ? 100 : 0;
	w->el.draw = window_draw;
	w->el.layer = 1; /* (3D: windows in front of the screen) */
	element_register(&w->el, self, vp);
	return self;
}

static void piece(Canvas *c, Bitmap *skin, Rect sr, int x, int y, int op)
{
	DrawOpts o = { .x = x, .y = y, .zx = 1, .zy = 1, .opacity = op };
	canvas_draw(c, skin, sr, &o);
}

/* a skin part stretched over a rect */
static void stretched(Canvas *c, Bitmap *skin, Rect sr, Rect d, int op, const Tone *tone)
{
	if (d.w <= 0 || d.h <= 0)
		return;
	DrawOpts o = { .x = d.x, .y = d.y, .zx = (double) d.w / sr.w, .zy = (double) d.h / sr.h,
	               .opacity = op, .tone = tone };
	canvas_draw(c, skin, sr, &o);
}

/* a skin part repeated over a rect (clipped to it) */
static void tiled(Canvas *c, Bitmap *skin, Rect sr, Rect d, int op, const Tone *tone)
{
	if (d.w <= 0 || d.h <= 0)
		return;
	Canvas sub = *c;
	int x0 = d.x > c->clip.x ? d.x : c->clip.x, y0 = d.y > c->clip.y ? d.y : c->clip.y;
	int x1 = d.x + d.w < c->clip.x + c->clip.w ? d.x + d.w : c->clip.x + c->clip.w;
	int y1 = d.y + d.h < c->clip.y + c->clip.h ? d.y + d.h : c->clip.y + c->clip.h;
	if (x0 >= x1 || y0 >= y1)
		return;
	sub.clip = (Rect) { x0, y0, x1 - x0, y1 - y0 };
	DrawOpts o = { .zx = 1, .zy = 1, .opacity = op, .tone = tone };
	for (int y = d.y; y < d.y + d.h; y += sr.h)
		for (int x = d.x; x < d.x + d.w; x += sr.w) {
			o.x = x;
			o.y = y;
			canvas_draw(&sub, skin, sr, &o);
		}
}

/* the cursor: 32x32 skin part with 8 pixel borders, stretched to the rect */
static void cursor(Canvas *c, Bitmap *skin, Rect r, int op)
{
	if (r.w <= 0 || r.h <= 0)
		return;
	const int b = 8;
	int sx = 64, sy = 64;
	piece(c, skin, (Rect) { sx, sy, b, b }, r.x, r.y, op);
	piece(c, skin, (Rect) { sx + 32 - b, sy, b, b }, r.x + r.w - b, r.y, op);
	piece(c, skin, (Rect) { sx, sy + 32 - b, b, b }, r.x, r.y + r.h - b, op);
	piece(c, skin, (Rect) { sx + 32 - b, sy + 32 - b, b, b }, r.x + r.w - b, r.y + r.h - b, op);
	stretched(c, skin, (Rect) { sx + b, sy, 32 - 2 * b, b }, (Rect) { r.x + b, r.y, r.w - 2 * b, b }, op, NULL);
	stretched(c, skin, (Rect) { sx + b, sy + 32 - b, 32 - 2 * b, b }, (Rect) { r.x + b, r.y + r.h - b, r.w - 2 * b, b }, op, NULL);
	stretched(c, skin, (Rect) { sx, sy + b, b, 32 - 2 * b }, (Rect) { r.x, r.y + b, b, r.h - 2 * b }, op, NULL);
	stretched(c, skin, (Rect) { sx + 32 - b, sy + b, b, 32 - 2 * b }, (Rect) { r.x + r.w - b, r.y + b, b, r.h - 2 * b }, op, NULL);
	stretched(c, skin, (Rect) { sx + b, sy + b, 32 - 2 * b, 32 - 2 * b }, (Rect) { r.x + b, r.y + b, r.w - 2 * b, r.h - 2 * b }, op, NULL);
}

static void window_draw(Element *e, Canvas *c, int vx, int vy)
{
	Window *w = (Window *) e;
	if (!w->visible || w->openness <= 0 || w->w <= 0 || w->h <= 0)
		return;
	int x = vx + w->x, y = vy + w->y;
	/* opening / closing: squeezed vertically around the middle */
	int h = w->h * w->openness / 255;
	int top = y + (w->h - h) / 2;
	Bitmap *skin = bitmap_get_maybe(w->windowskin);
	if (skin && skin->w >= 128 && skin->h >= 128 && h > 0) {
		int op = w->opacity;
		const Tone *tone = tone_get(w->tone);
		/* background: stretched part + repeated pattern */
		Rect in = { x + 2, top + 2, w->w - 4, h - 4 };
		stretched(c, skin, (Rect) { 0, 0, 64, 64 }, in, w->back_opacity * op / 255, tone);
		tiled(c, skin, (Rect) { 0, 64, 64, 64 }, in, w->back_opacity * op / 255, tone);
		/* frame: corners and edges */
		if (h >= 32) {
			piece(c, skin, (Rect) { 64, 0, 16, 16 }, x, top, op);
			piece(c, skin, (Rect) { 112, 0, 16, 16 }, x + w->w - 16, top, op);
			piece(c, skin, (Rect) { 64, 48, 16, 16 }, x, top + h - 16, op);
			piece(c, skin, (Rect) { 112, 48, 16, 16 }, x + w->w - 16, top + h - 16, op);
			tiled(c, skin, (Rect) { 80, 0, 32, 16 }, (Rect) { x + 16, top, w->w - 32, 16 }, op, NULL);
			tiled(c, skin, (Rect) { 80, 48, 32, 16 }, (Rect) { x + 16, top + h - 16, w->w - 32, 16 }, op, NULL);
			tiled(c, skin, (Rect) { 64, 16, 16, 32 }, (Rect) { x, top + 16, 16, h - 32 }, op, NULL);
			tiled(c, skin, (Rect) { 112, 16, 16, 32 }, (Rect) { x + w->w - 16, top + 16, 16, h - 32 }, op, NULL);
		}
	}
	if (w->openness < 255)
		return;
	/* inner area: contents, cursor, arrows */
	Rect inner = { x + w->padding, y + w->padding, w->w - 2 * w->padding, w->h - w->padding - w->padding_bottom };
	Canvas sub = *c;
	int x0 = inner.x > c->clip.x ? inner.x : c->clip.x, y0 = inner.y > c->clip.y ? inner.y : c->clip.y;
	int x1 = inner.x + inner.w < c->clip.x + c->clip.w ? inner.x + inner.w : c->clip.x + c->clip.w;
	int y1 = inner.y + inner.h < c->clip.y + c->clip.h ? inner.y + inner.h : c->clip.y + c->clip.h;
	if (x0 < x1 && y0 < y1) {
		sub.clip = (Rect) { x0, y0, x1 - x0, y1 - y0 };
		Rect cr = *rect_get(w->cursor_rect);
		if (skin && cr.w > 0 && cr.h > 0) {
			/* blinks while active */
			int phase = w->frame % 40, op = 255;
			if (w->active)
				op = 255 - (phase < 20 ? phase : 40 - phase) * 6;
			else
				op = 160;
			cursor(&sub, skin, (Rect) { inner.x + cr.x - w->ox, inner.y + cr.y - w->oy, cr.w, cr.h }, op * w->contents_opacity / 255);
		}
		Bitmap *cont = bitmap_get_maybe(w->contents);
		if (cont) {
			DrawOpts o = { .x = inner.x - w->ox, .y = inner.y - w->oy, .zx = 1, .zy = 1, .opacity = w->contents_opacity };
			canvas_draw(&sub, cont, (Rect) { 0, 0, cont->w, cont->h }, &o);
			if (cont->nops && sub.gpu)
				gpu_text_layer(cont, inner.x - w->ox, inner.y - w->oy, sub.clip, w->contents_opacity);
		}
	}
	if (!skin)
		return;
	Bitmap *cont = bitmap_get_maybe(w->contents);
	if (w->arrows_visible && cont) {
		int op = w->opacity;
		if (w->oy > 0)
			piece(c, skin, (Rect) { 88, 16, 16, 8 }, x + w->w / 2 - 8, y + 4, op);
		if (cont->h - w->oy > inner.h)
			piece(c, skin, (Rect) { 88, 40, 16, 8 }, x + w->w / 2 - 8, y + w->h - 12, op);
		if (w->ox > 0)
			piece(c, skin, (Rect) { 80, 24, 8, 16 }, x + 4, y + w->h / 2 - 8, op);
		if (cont->w - w->ox > inner.w)
			piece(c, skin, (Rect) { 104, 24, 8, 16 }, x + w->w - 12, y + w->h / 2 - 8, op);
	}
	if (w->pause) {
		int f = (w->frame / 8) % 4;
		piece(c, skin, (Rect) { 96 + (f % 2) * 16, 64 + (f / 2) * 16, 16, 16 },
		      x + w->w / 2 - 8, y + w->h - 16, w->opacity);
	}
}

static VALUE window_dispose(VALUE self)
{
	Window *w = rb_check_typeddata(self, &window_type);
	if (!w->disposed) {
		w->disposed = true;
		element_unregister(&w->el);
	}
	return Qnil;
}
static VALUE window_disposed(VALUE self) { return ((Window *) rb_check_typeddata(self, &window_type))->disposed ? Qtrue : Qfalse; }
static VALUE window_update(VALUE self) { window_get(self)->frame++; return Qnil; }
static VALUE window_move(VALUE self, VALUE x, VALUE y, VALUE wd, VALUE ht)
{
	Window *w = window_get(self);
	w->x = num2int(x); w->y = num2int(y); w->w = num2int(wd); w->h = num2int(ht);
	return Qnil;
}
static VALUE window_is_open(VALUE self) { return window_get(self)->openness == 255 ? Qtrue : Qfalse; }
static VALUE window_is_close(VALUE self) { return window_get(self)->openness == 0 ? Qtrue : Qfalse; }

#define W_VAL(f) \
	static VALUE window_get_##f(VALUE self) { return window_get(self)->f; } \
	static VALUE window_set_##f(VALUE self, VALUE v) { window_get(self)->f = v; return v; }
W_VAL(windowskin)
static VALUE window_get_contents(VALUE self) { return window_get(self)->contents; }
/* (contents get sharp text: settings "Window text") */
static VALUE window_set_contents(VALUE self, VALUE v)
{
	window_get(self)->contents = v;
	Bitmap *b = settings.sharp_text && gpu_on ? bitmap_get_maybe(v) : NULL;
	if (b)
		b->text_native = true;
	return v;
}
static VALUE window_cursor_rect(VALUE self) { return window_get(self)->cursor_rect; }
static VALUE window_set_cursor_rect(VALUE self, VALUE r) { *rect_get(window_get(self)->cursor_rect) = *rect_get(r); return r; }
static VALUE window_tone(VALUE self) { return window_get(self)->tone; }
static VALUE window_set_tone(VALUE self, VALUE t) { *tone_get(window_get(self)->tone) = *tone_get(t); return t; }
static VALUE window_viewport(VALUE self) { return window_get(self)->el.viewport; }
static VALUE window_set_viewport(VALUE self, VALUE v) { window_get(self)->el.viewport = v; return v; }
static VALUE window_z(VALUE self) { return INT2NUM(window_get(self)->el.z); }
static VALUE window_set_z(VALUE self, VALUE z) { window_get(self)->el.z = num2int(z); return z; }
#define W_INT(f, lo, hi) \
	static VALUE window_get_##f(VALUE self) { return INT2NUM(window_get(self)->f); } \
	static VALUE window_set_##f(VALUE self, VALUE v) { int n = num2int(v); window_get(self)->f = CLAMP(n, lo, hi); return v; }
W_INT(x, -999999, 999999) W_INT(y, -999999, 999999) W_INT(w, 0, 999999) W_INT(h, 0, 999999)
W_INT(ox, -999999, 999999) W_INT(oy, -999999, 999999) W_INT(padding_bottom, 0, 999999)
W_INT(opacity, 0, 255) W_INT(back_opacity, 0, 255) W_INT(contents_opacity, 0, 255) W_INT(openness, 0, 255)
static VALUE window_get_padding(VALUE self) { return INT2NUM(window_get(self)->padding); }
static VALUE window_set_padding(VALUE self, VALUE v)
{
	Window *w = window_get(self);
	w->padding = w->padding_bottom = num2int(v);
	return v;
}
#define W_BOOL(f) \
	static VALUE window_get_##f(VALUE self) { return window_get(self)->f ? Qtrue : Qfalse; } \
	static VALUE window_set_##f(VALUE self, VALUE v) { window_get(self)->f = RTEST(v); return v; }
W_BOOL(visible) W_BOOL(active) W_BOOL(pause) W_BOOL(arrows_visible)

void init_window(void)
{
	cWindow = rb_define_class("Window", rb_cObject);
	rb_define_alloc_func(cWindow, window_alloc);
	rb_define_method(cWindow, "initialize", window_init, -1);
	rb_define_method(cWindow, "dispose", window_dispose, 0);
	rb_define_method(cWindow, "disposed?", window_disposed, 0);
	rb_define_method(cWindow, "update", window_update, 0);
	rb_define_method(cWindow, "mkxpz_wide=", window_set_wide, 1);
	rb_define_method(cWindow, "move", window_move, 4);
	rb_define_method(cWindow, "open?", window_is_open, 0);
	rb_define_method(cWindow, "close?", window_is_close, 0);
	DEF_ATTR(cWindow, "windowskin", window_get_windowskin, window_set_windowskin);
	DEF_ATTR(cWindow, "contents", window_get_contents, window_set_contents);
	DEF_ATTR(cWindow, "cursor_rect", window_cursor_rect, window_set_cursor_rect);
	DEF_ATTR(cWindow, "tone", window_tone, window_set_tone);
	DEF_ATTR(cWindow, "viewport", window_viewport, window_set_viewport);
	DEF_ATTR(cWindow, "z", window_z, window_set_z);
	DEF_ATTR(cWindow, "x", window_get_x, window_set_x);
	DEF_ATTR(cWindow, "y", window_get_y, window_set_y);
	DEF_ATTR(cWindow, "width", window_get_w, window_set_w);
	DEF_ATTR(cWindow, "height", window_get_h, window_set_h);
	DEF_ATTR(cWindow, "ox", window_get_ox, window_set_ox);
	DEF_ATTR(cWindow, "oy", window_get_oy, window_set_oy);
	DEF_ATTR(cWindow, "padding", window_get_padding, window_set_padding);
	DEF_ATTR(cWindow, "padding_bottom", window_get_padding_bottom, window_set_padding_bottom);
	DEF_ATTR(cWindow, "opacity", window_get_opacity, window_set_opacity);
	DEF_ATTR(cWindow, "back_opacity", window_get_back_opacity, window_set_back_opacity);
	DEF_ATTR(cWindow, "contents_opacity", window_get_contents_opacity, window_set_contents_opacity);
	DEF_ATTR(cWindow, "openness", window_get_openness, window_set_openness);
	DEF_ATTR(cWindow, "visible", window_get_visible, window_set_visible);
	DEF_ATTR(cWindow, "active", window_get_active, window_set_active);
	DEF_ATTR(cWindow, "pause", window_get_pause, window_set_pause);
	DEF_ATTR(cWindow, "arrows_visible", window_get_arrows_visible, window_set_arrows_visible);
}
