/* Rect, Color, Tone and Table, with RGSS's Marshal formats (the game's data
 * files and saves contain them). */
#include "engine.h"
#include <string.h>

VALUE cRect, cColor, cTone, cTable;

int num2int(VALUE v)
{
	return RB_FLOAT_TYPE_P(v) ? (int) RFLOAT_VALUE(v) : NUM2INT(v);
}

double num2dbl(VALUE v)
{
	return NUM2DBL(v);
}

#define SIMPLE_TYPE(T, name) \
	static const rb_data_type_t name##_type = { #name, { NULL, RUBY_TYPED_DEFAULT_FREE, NULL }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY }; \
	static VALUE name##_alloc(VALUE klass) { T *p; return TypedData_Make_Struct(klass, T, &name##_type, p); } \
	T *name##_get(VALUE v) { return rb_check_typeddata(v, &name##_type); }

/* --- Rect */
SIMPLE_TYPE(Rect, rect)

VALUE rect_new(int x, int y, int w, int h)
{
	VALUE v = rect_alloc(cRect);
	Rect *r = rect_get(v);
	r->x = x; r->y = y; r->w = w; r->h = h;
	return v;
}

static VALUE rect_set(int argc, VALUE *argv, VALUE self)
{
	Rect *r = rect_get(self);
	if (argc == 1) {
		*r = *rect_get(argv[0]);
	} else if (argc == 4) {
		r->x = num2int(argv[0]); r->y = num2int(argv[1]);
		r->w = num2int(argv[2]); r->h = num2int(argv[3]);
	} else if (argc != 0) {
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 0, 1 or 4)", argc);
	}
	return self;
}
static VALUE rect_empty(VALUE self) { Rect *r = rect_get(self); r->x = r->y = r->w = r->h = 0; return self; }
#define RECT_ATTR(f, name) \
	static VALUE rect_get_##f(VALUE s) { return INT2NUM(rect_get(s)->f); } \
	static VALUE rect_set_##f(VALUE s, VALUE v) { rect_get(s)->f = num2int(v); return v; }
RECT_ATTR(x, x) RECT_ATTR(y, y) RECT_ATTR(w, width) RECT_ATTR(h, height)
static VALUE rect_eq(VALUE self, VALUE o)
{
	if (!rb_obj_is_kind_of(o, cRect)) return Qfalse;
	return memcmp(rect_get(self), rect_get(o), sizeof(Rect)) ? Qfalse : Qtrue;
}
static VALUE rect_to_s(VALUE self)
{
	Rect *r = rect_get(self);
	return rb_sprintf("(%d, %d, %d, %d)", r->x, r->y, r->w, r->h);
}
static VALUE rect_dump(VALUE self, VALUE depth)
{
	Rect *r = rect_get(self);
	int32_t d[4] = { r->x, r->y, r->w, r->h };
	return rb_str_new((const char *) d, sizeof d);
}
static VALUE rect_load(VALUE klass, VALUE s)
{
	int32_t d[4] = {0};
	memcpy(d, RSTRING_PTR(s), RSTRING_LEN(s) < (long) sizeof d ? RSTRING_LEN(s) : (long) sizeof d);
	return rect_new(d[0], d[1], d[2], d[3]);
}

/* --- Color (0..255 each) */
SIMPLE_TYPE(Color, color)

static double clampc(double v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

VALUE color_new(double r, double g, double b, double a)
{
	VALUE v = color_alloc(cColor);
	Color *c = color_get(v);
	c->r = clampc(r); c->g = clampc(g); c->b = clampc(b); c->a = clampc(a);
	return v;
}
static VALUE color_set(int argc, VALUE *argv, VALUE self)
{
	Color *c = color_get(self);
	if (argc == 1) {
		*c = *color_get(argv[0]);
	} else if (argc == 3 || argc == 4) {
		c->r = clampc(num2dbl(argv[0])); c->g = clampc(num2dbl(argv[1]));
		c->b = clampc(num2dbl(argv[2])); c->a = argc == 4 ? clampc(num2dbl(argv[3])) : 255;
	} else if (argc != 0) {
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 0, 1, 3 or 4)", argc);
	}
	return self;
}
#define COLOR_ATTR(f, name) \
	static VALUE color_get_##f(VALUE s) { return DBL2NUM(color_get(s)->f); } \
	static VALUE color_set_##f(VALUE s, VALUE v) { color_get(s)->f = clampc(num2dbl(v)); return v; }
COLOR_ATTR(r, red) COLOR_ATTR(g, green) COLOR_ATTR(b, blue) COLOR_ATTR(a, alpha)
static VALUE color_eq(VALUE self, VALUE o)
{
	if (!rb_obj_is_kind_of(o, cColor)) return Qfalse;
	return memcmp(color_get(self), color_get(o), sizeof(Color)) ? Qfalse : Qtrue;
}
static VALUE color_to_s(VALUE self)
{
	Color *c = color_get(self);
	return rb_sprintf("(%f, %f, %f, %f)", c->r, c->g, c->b, c->a);
}
static VALUE color_dump(VALUE self, VALUE depth)
{
	Color *c = color_get(self);
	double d[4] = { c->r, c->g, c->b, c->a };
	return rb_str_new((const char *) d, sizeof d);
}
static VALUE color_load(VALUE klass, VALUE s)
{
	double d[4] = {0};
	memcpy(d, RSTRING_PTR(s), RSTRING_LEN(s) < (long) sizeof d ? RSTRING_LEN(s) : (long) sizeof d);
	return color_new(d[0], d[1], d[2], d[3]);
}

/* --- Tone (-255..255, gray 0..255) */
SIMPLE_TYPE(Tone, tone)

static double clampt(double v) { return v < -255 ? -255 : v > 255 ? 255 : v; }

VALUE tone_new(double r, double g, double b, double gray)
{
	VALUE v = tone_alloc(cTone);
	Tone *t = tone_get(v);
	t->r = clampt(r); t->g = clampt(g); t->b = clampt(b); t->gray = clampc(gray);
	return v;
}
static VALUE tone_set(int argc, VALUE *argv, VALUE self)
{
	Tone *t = tone_get(self);
	if (argc == 1) {
		*t = *tone_get(argv[0]);
	} else if (argc == 3 || argc == 4) {
		t->r = clampt(num2dbl(argv[0])); t->g = clampt(num2dbl(argv[1]));
		t->b = clampt(num2dbl(argv[2])); t->gray = argc == 4 ? clampc(num2dbl(argv[3])) : 0;
	} else if (argc != 0) {
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 0, 1, 3 or 4)", argc);
	}
	return self;
}
#define TONE_ATTR(f, name, cl) \
	static VALUE tone_get_##f(VALUE s) { return DBL2NUM(tone_get(s)->f); } \
	static VALUE tone_set_##f(VALUE s, VALUE v) { tone_get(s)->f = cl(num2dbl(v)); return v; }
TONE_ATTR(r, red, clampt) TONE_ATTR(g, green, clampt) TONE_ATTR(b, blue, clampt) TONE_ATTR(gray, gray, clampc)
static VALUE tone_eq(VALUE self, VALUE o)
{
	if (!rb_obj_is_kind_of(o, cTone)) return Qfalse;
	return memcmp(tone_get(self), tone_get(o), sizeof(Tone)) ? Qfalse : Qtrue;
}
static VALUE tone_to_s(VALUE self)
{
	Tone *t = tone_get(self);
	return rb_sprintf("(%f, %f, %f, %f)", t->r, t->g, t->b, t->gray);
}
static VALUE tone_dump(VALUE self, VALUE depth)
{
	Tone *t = tone_get(self);
	double d[4] = { t->r, t->g, t->b, t->gray };
	return rb_str_new((const char *) d, sizeof d);
}
static VALUE tone_load(VALUE klass, VALUE s)
{
	double d[4] = {0};
	memcpy(d, RSTRING_PTR(s), RSTRING_LEN(s) < (long) sizeof d ? RSTRING_LEN(s) : (long) sizeof d);
	return tone_new(d[0], d[1], d[2], d[3]);
}

/* --- Table: up to 3 dimensions of int16 */
static void table_free(void *p)
{
	Table *t = p;
	xfree(t->data);
	xfree(t);
}
static size_t table_size(const void *p)
{
	const Table *t = p;
	return sizeof *t + (size_t) t->xs * t->ys * t->zs * 2;
}
static const rb_data_type_t table_type = { "Table", { NULL, table_free, table_size }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY };
static VALUE table_alloc(VALUE klass)
{
	Table *t;
	VALUE v = TypedData_Make_Struct(klass, Table, &table_type, t);
	t->dim = 1; t->xs = t->ys = t->zs = 1;
	t->data = ZALLOC_N(int16_t, 1);
	return v;
}
Table *table_get(VALUE v) { return rb_check_typeddata(v, &table_type); }

static void table_resize_to(Table *t, int dim, int xs, int ys, int zs)
{
	xs = xs < 0 ? 0 : xs; ys = ys < 1 ? 1 : ys; zs = zs < 1 ? 1 : zs;
	int16_t *n = ZALLOC_N(int16_t, (size_t) xs * ys * zs + 1);
	for (int z = 0; z < zs && z < t->zs; z++)
		for (int y = 0; y < ys && y < t->ys; y++)
			for (int x = 0; x < xs && x < t->xs; x++)
				n[x + xs * (y + ys * z)] = t->data[x + t->xs * (y + t->ys * z)];
	xfree(t->data);
	t->data = n;
	t->dim = dim; t->xs = xs; t->ys = ys; t->zs = zs;
	t->version++;
}
static VALUE table_init(int argc, VALUE *argv, VALUE self)
{
	if (argc < 1 || argc > 3)
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 1..3)", argc);
	table_resize_to(table_get(self), argc, num2int(argv[0]), argc > 1 ? num2int(argv[1]) : 1,
	                argc > 2 ? num2int(argv[2]) : 1);
	return self;
}
static VALUE table_resize(int argc, VALUE *argv, VALUE self) { return table_init(argc, argv, self); }
static VALUE table_xs(VALUE s) { return INT2NUM(table_get(s)->xs); }
static VALUE table_ys(VALUE s) { return INT2NUM(table_get(s)->ys); }
static VALUE table_zs(VALUE s) { return INT2NUM(table_get(s)->zs); }
static VALUE table_aref(int argc, VALUE *argv, VALUE self)
{
	Table *t = table_get(self);
	int x = argc > 0 ? num2int(argv[0]) : 0, y = argc > 1 ? num2int(argv[1]) : 0, z = argc > 2 ? num2int(argv[2]) : 0;
	if (x < 0 || y < 0 || z < 0 || x >= t->xs || y >= t->ys || z >= t->zs)
		return Qnil;
	return INT2FIX(t->data[x + t->xs * (y + t->ys * z)]);
}
static VALUE table_aset(int argc, VALUE *argv, VALUE self)
{
	if (argc < 2)
		rb_raise(rb_eArgError, "wrong number of arguments");
	Table *t = table_get(self);
	int x = num2int(argv[0]), y = argc > 2 ? num2int(argv[1]) : 0, z = argc > 3 ? num2int(argv[2]) : 0;
	VALUE v = argv[argc - 1];
	if (x >= 0 && y >= 0 && z >= 0 && x < t->xs && y < t->ys && z < t->zs) {
		t->data[x + t->xs * (y + t->ys * z)] = (int16_t) num2int(v);
		t->version++;
	}
	return v;
}
static VALUE table_dump(VALUE self, VALUE depth)
{
	Table *t = table_get(self);
	size_t n = (size_t) t->xs * t->ys * t->zs;
	int32_t head[5] = { t->dim, t->xs, t->ys, t->zs, (int32_t) n };
	VALUE s = rb_str_new((const char *) head, sizeof head);
	rb_str_cat(s, (const char *) t->data, n * 2);
	return s;
}
static VALUE table_load(VALUE klass, VALUE s)
{
	if (RSTRING_LEN(s) < 20)
		rb_raise(rb_eArgError, "Table: broken data");
	int32_t head[5];
	memcpy(head, RSTRING_PTR(s), sizeof head);
	VALUE v = table_alloc(klass);
	Table *t = table_get(v);
	table_resize_to(t, head[0], head[1], head[2], head[3]);
	size_t n = (size_t) t->xs * t->ys * t->zs;
	size_t have = (RSTRING_LEN(s) - 20) / 2;
	memcpy(t->data, RSTRING_PTR(s) + 20, (n < have ? n : have) * 2);
	return v;
}

void init_types(void)
{
	cRect = rb_define_class("Rect", rb_cObject);
	rb_define_alloc_func(cRect, rect_alloc);
	rb_define_method(cRect, "initialize", rect_set, -1);
	rb_define_method(cRect, "set", rect_set, -1);
	rb_define_method(cRect, "empty", rect_empty, 0);
	DEF_ATTR(cRect, "x", rect_get_x, rect_set_x);
	DEF_ATTR(cRect, "y", rect_get_y, rect_set_y);
	DEF_ATTR(cRect, "width", rect_get_w, rect_set_w);
	DEF_ATTR(cRect, "height", rect_get_h, rect_set_h);
	rb_define_method(cRect, "==", rect_eq, 1);
	rb_define_method(cRect, "to_s", rect_to_s, 0);
	rb_define_method(cRect, "_dump", rect_dump, 1);
	rb_define_singleton_method(cRect, "_load", rect_load, 1);

	cColor = rb_define_class("Color", rb_cObject);
	rb_define_alloc_func(cColor, color_alloc);
	rb_define_method(cColor, "initialize", color_set, -1);
	rb_define_method(cColor, "set", color_set, -1);
	DEF_ATTR(cColor, "red", color_get_r, color_set_r);
	DEF_ATTR(cColor, "green", color_get_g, color_set_g);
	DEF_ATTR(cColor, "blue", color_get_b, color_set_b);
	DEF_ATTR(cColor, "alpha", color_get_a, color_set_a);
	rb_define_method(cColor, "==", color_eq, 1);
	rb_define_method(cColor, "to_s", color_to_s, 0);
	rb_define_method(cColor, "_dump", color_dump, 1);
	rb_define_singleton_method(cColor, "_load", color_load, 1);

	cTone = rb_define_class("Tone", rb_cObject);
	rb_define_alloc_func(cTone, tone_alloc);
	rb_define_method(cTone, "initialize", tone_set, -1);
	rb_define_method(cTone, "set", tone_set, -1);
	DEF_ATTR(cTone, "red", tone_get_r, tone_set_r);
	DEF_ATTR(cTone, "green", tone_get_g, tone_set_g);
	DEF_ATTR(cTone, "blue", tone_get_b, tone_set_b);
	DEF_ATTR(cTone, "gray", tone_get_gray, tone_set_gray);
	rb_define_method(cTone, "==", tone_eq, 1);
	rb_define_method(cTone, "to_s", tone_to_s, 0);
	rb_define_method(cTone, "_dump", tone_dump, 1);
	rb_define_singleton_method(cTone, "_load", tone_load, 1);

	cTable = rb_define_class("Table", rb_cObject);
	rb_define_alloc_func(cTable, table_alloc);
	rb_define_method(cTable, "initialize", table_init, -1);
	rb_define_method(cTable, "resize", table_resize, -1);
	rb_define_method(cTable, "xsize", table_xs, 0);
	rb_define_method(cTable, "ysize", table_ys, 0);
	rb_define_method(cTable, "zsize", table_zs, 0);
	rb_define_method(cTable, "[]", table_aref, -1);
	rb_define_method(cTable, "[]=", table_aset, -1);
	rb_define_method(cTable, "_dump", table_dump, 1);
	rb_define_singleton_method(cTable, "_load", table_load, 1);
}
