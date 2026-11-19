/* Tilemap (VX / VX Ace): drawn with the GPU straight from the tileset
 * bitmaps (A1..A5, B..E). Tile IDs: http://www.tktkgame.com/tkool/memo/vx/tile_id.html
 * Two layers: the ground (z 0) and tiles above the characters (z 200). */
#include "engine.h"
#include "autotiles.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

VALUE cTilemap;

/* the visible tiles as vertex buffers: batches by layer and bitmap, in
 * drawing order; rebuilt when the visible tile area, the animation frame,
 * the map data or the bitmaps change */
typedef struct {
	int bitmap;  /* 0..8, or -1: shadows (untextured) */
	bool above;
	int first, count;
} Batch;

typedef struct {
	Element ground, above;
	VALUE bitmaps, map_data, flash_data, flags;
	bool visible, disposed;
	int ox, oy;
	int frame; /* animation */
	/* the cache */
	float *verts;
	size_t cap;
	Batch batches[64];
	int nbatches;
	int key[12]; /* what the cache was built for */
	bool built;
} Tilemap;

static void tilemap_draw_ground(Element *e, Canvas *c, int ox, int oy);
static void tilemap_draw_above(Element *e, Canvas *c, int ox, int oy);

static void tilemap_mark(void *p)
{
	Tilemap *t = p;
	rb_gc_mark(t->bitmaps);
	rb_gc_mark(t->map_data);
	rb_gc_mark(t->flash_data);
	rb_gc_mark(t->flags);
	rb_gc_mark(t->ground.viewport);
}
static void tilemap_free(void *p)
{
	Tilemap *t = p;
	if (!t->disposed) {
		element_unregister(&t->ground);
		element_unregister(&t->above);
	}
	gpu_free(t->verts);
	xfree(t);
}
static const rb_data_type_t tilemap_type = { "Tilemap", { tilemap_mark, tilemap_free, NULL }, 0, 0, 0 };
static VALUE tilemap_alloc(VALUE klass)
{
	Tilemap *t;
	VALUE self = TypedData_Make_Struct(klass, Tilemap, &tilemap_type, t);
	t->bitmaps = t->map_data = t->flash_data = t->flags = Qnil;
	t->ground.viewport = t->above.viewport = Qnil;
	return self;
}
static Tilemap *tilemap_get(VALUE self)
{
	Tilemap *t = rb_check_typeddata(self, &tilemap_type);
	if (t->disposed)
		rb_raise(eRGSSError, "disposed tilemap");
	return t;
}

static VALUE tilemap_init(int argc, VALUE *argv, VALUE self)
{
	Tilemap *t = rb_check_typeddata(self, &tilemap_type);
	VALUE vp = argc > 0 ? argv[0] : Qnil;
	t->bitmaps = rb_ary_new_capa(9);
	for (int i = 0; i < 9; i++)
		rb_ary_push(t->bitmaps, Qnil);
	t->visible = true;
	t->ground.z = 0;
	t->ground.draw = tilemap_draw_ground;
	t->above.z = 200;
	t->above.draw = tilemap_draw_above;
	element_register(&t->ground, self, vp);
	element_register(&t->above, self, vp);
	return self;
}

/* --- reading tiles into batches */
typedef struct {
	float *v;      /* growing vertex list of the current batch */
	size_t n, cap; /* floats */
} List;

typedef struct {
	Tilemap *t;
	Bitmap *bm[9];
	float tw[9], th[9]; /* their textures' sizes */
	List lists[9];      /* this layer's quads per bitmap */
	List shadow;
	int ani_a, ani_c;
	bool above;
} Ctx;

static int pow2(int n)
{
	int p = 8;
	while (p < n)
		p <<= 1;
	return p;
}

static void push_vertex(List *l, float x, float y, float u, float v, float a)
{
	if (l->n + 9 > l->cap) {
		size_t nc = l->cap ? l->cap * 2 : 9 * 6 * 64;
		float *nv = realloc(l->v, nc * sizeof(float));
		if (!nv)
			return;
		l->v = nv;
		l->cap = nc;
	}
	float *p = l->v + l->n;
	p[0] = x; p[1] = y; p[2] = 0.5f; p[3] = u; p[4] = v;
	p[5] = 0; p[6] = 0; p[7] = 0; p[8] = a;
	if (u >= 0) /* textured: white */
		p[5] = p[6] = p[7] = 1;
	l->n += 9;
}

static void push_quad(List *l, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float a)
{
	push_vertex(l, x0, y0, u0, v0, a);
	push_vertex(l, x0, y1, u0, v1, a);
	push_vertex(l, x1, y0, u1, v0, a);
	push_vertex(l, x1, y0, u1, v0, a);
	push_vertex(l, x0, y1, u0, v1, a);
	push_vertex(l, x1, y1, u1, v1, a);
}

/* source rect in bitmap i -> destination rect (map pixels, relative to the
 * cache's first tile); half a pixel inside the source (no bleeding) */
static void put(Ctx *x, int i, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
	if (!x->bm[i] || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	float tw = x->tw[i], th = x->th[i];
	push_quad(&x->lists[i], dx, dy, dx + dw, dy + dh, (sx + 0.5f) / tw, 1 - (sy + 0.5f) / th,
	          (sx + sw - 0.5f) / tw, 1 - (sy + sh - 0.5f) / th, 1);
}

/* an A-type autotile (4 quarters) from block origin (bx, by) in pixels */
static void autotile_a(Ctx *x, int i, int bx, int by, int p, float dx, float dy)
{
	if (p >= 48)
		return;
	for (int q = 0; q < 4; q++) {
		const TileRect *r = &at_RectsA[p * 4 + q];
		put(x, i, bx + r->x, by + r->y, r->w, r->h, dx + (q & 1) * 16, dy + (q >> 1) * 16, 16, 16);
	}
}

/* the "table" variant (A2): 6 parts, the last two are the table's legs */
static void autotile_table(Ctx *x, int i, int bx, int by, int p, float dx, float dy)
{
	if (p >= 48)
		return;
	static const signed char px[6] = { 0, 16, 0, 16, 0, 16 }, py[6] = { 0, 0, 16, 16, 24, 24 };
	for (int q = 0; q < 6; q++) {
		const TileRect *r = &at_RectsA2[p * 6 + q];
		float size = at_RectsA2Sizes[p * 6 + q];
		put(x, i, bx + r->x, by + r->y, r->w, r->h, dx + px[q], dy + py[q], size, size);
	}
}

/* a B-type autotile (walls) */
static void autotile_b(Ctx *x, int i, int bx, int by, int p, float dx, float dy)
{
	if (p >= 16)
		return;
	for (int q = 0; q < 4; q++) {
		const TileRect *r = &at_RectsB[p * 4 + q];
		put(x, i, bx + r->x, by + r->y, r->w, r->h, dx + (q & 1) * 16, dy + (q >> 1) * 16, 16, 16);
	}
}

/* waterfalls: two 16x32 halves */
static void autotile_c(Ctx *x, int i, int bx, int by, int p, float dx, float dy)
{
	if (p > 3)
		return;
	for (int q = 0; q < 2; q++) {
		const TileRect *r = &at_RectsC[p * 2 + q];
		put(x, i, bx + r->x, by + r->y + x->ani_c * 32, r->w, r->h, dx + q * 16, dy, 16, 32);
	}
}

static int flag_of(Tilemap *t, int id)
{
	if (NIL_P(t->flags))
		return 0;
	Table *f = table_get(t->flags);
	return id >= 0 && id < f->xs ? f->data[id] : 0;
}

/* one tile: into the ground or the above batches */
static void tile(Ctx *x, int id, float dx, float dy)
{
	Tilemap *t = x->t;
	int flag = flag_of(t, id);
	bool over = flag & 0x10;
	if (id < 0x0400) { /* B..E */
		if (over != x->above)
			return;
		int n = id % 256;
		put(x, 5 + id / 256, (n % 8 + ((n / 128) % 2) * 8) * 32, ((n / 8) % 16) * 32, 32, 32, dx, dy, 32, 32);
		return;
	}
	if (id >= 0x0600 && id < 0x0680) { /* A5 */
		if (over != x->above)
			return;
		int n = id - 0x0600;
		put(x, 4, (n % 8) * 32, (n / 8) * 32, 32, 32, dx, dy, 32, 32);
		return;
	}
	if (x->above || id < 0x0800)
		return; /* autotiles: always on the ground */
	int n, p, k;
	if (id < 0x0B00) { /* A1: water, waterfalls */
		static const struct { signed char x, y; char kind; } a1[16] = {
			{ 0, 0, 'a' }, { 0, 3, 'a' }, { 6, 0, 's' }, { 6, 3, 's' }, { 8, 0, 'a' }, { 14, 0, 'c' },
			{ 8, 3, 'a' }, { 14, 3, 'c' }, { 0, 6, 'a' }, { 6, 6, 'c' }, { 0, 9, 'a' }, { 6, 9, 'c' },
			{ 8, 6, 'a' }, { 14, 6, 'c' }, { 8, 9, 'a' }, { 14, 9, 'c' },
		};
		n = id - 0x0800;
		p = n % 48;
		k = n / 48;
		int bx = a1[k].x * 32, by = a1[k].y * 32;
		if (a1[k].kind == 'c')
			autotile_c(x, 0, bx, by, p, dx, dy);
		else
			autotile_a(x, 0, bx + (a1[k].kind == 'a' ? x->ani_a * 64 : 0), by, p, dx, dy);
		return;
	}
	if (id < 0x1100) { /* A2: ground (or tables) */
		n = id - 0x0B00;
		p = n % 48;
		k = n / 48;
		bool table = rgss_version >= 3 ? (flag & 0x80) : (n % (8 * 48)) >= 7 * 48;
		int bx = (k % 8) * 64, by = (k / 8) * 96;
		if (table)
			autotile_table(x, 1, bx, by, p, dx, dy);
		else
			autotile_a(x, 1, bx, by, p, dx, dy);
		return;
	}
	if (id < 0x1700) { /* A3: building walls and roofs */
		n = id - 0x1100;
		p = n % 48;
		k = n / 48;
		autotile_b(x, 2, (k % 8) * 64, (k / 8) * 64, p, dx, dy);
		return;
	}
	if (id < 0x2000) { /* A4: wall tops (A-type) and walls (B-type) */
		static const int off_y[] = { 0, 3, 5, 8, 10, 13 };
		n = id - 0x1700;
		p = n % 48;
		k = n / 48;
		int row = k / 8;
		if (row > 5)
			return;
		int bx = (k % 8) * 64, by = off_y[row] * 32;
		if (row % 2 == 0)
			autotile_a(x, 3, bx, by, p, dx, dy);
		else
			autotile_b(x, 3, bx, by, p, dx, dy);
	}
}

static int wrap(int v, int n)
{
	return n > 0 ? ((v % n) + n) % n : 0;
}

/* this layer's lists -> batches (appended to the vertex array) */
static void flush_lists(Tilemap *t, Ctx *x, float **all, size_t *n, size_t *cap, bool above)
{
	for (int i = -1; i < 9; i++) {
		List *l = i < 0 ? &x->shadow : &x->lists[i];
		if (!l->n)
			continue;
		if (*n + l->n > *cap) {
			size_t nc = (*n + l->n) * 2;
			float *nv = realloc(*all, nc * sizeof(float));
			if (!nv)
				return;
			*all = nv;
			*cap = nc;
		}
		if (t->nbatches < (int) (sizeof t->batches / sizeof *t->batches)) {
			t->batches[t->nbatches++] = (Batch) { i, above, (int) (*n / 9), (int) (l->n / 9) };
			memcpy(*all + *n, l->v, l->n * sizeof(float));
			*n += l->n;
		}
		l->n = 0;
	}
}

/* build the cache for tiles (x0, y0)..(x1, y1); vertices relative to x0, y0 */
static void build(Tilemap *t, Table *m, int x0, int y0, int x1, int y1, int ani_a, int ani_c)
{
	Ctx x;
	memset(&x, 0, sizeof x);
	x.t = t;
	x.ani_a = ani_a;
	x.ani_c = ani_c;
	for (int i = 0; i < 9; i++) {
		x.bm[i] = bitmap_get_maybe(rb_ary_entry(t->bitmaps, i));
		if (x.bm[i] && (x.bm[i]->w > 1024 || x.bm[i]->h > 1024))
			x.bm[i] = NULL;
		if (x.bm[i]) {
			x.tw[i] = pow2(x.bm[i]->w);
			x.th[i] = pow2(x.bm[i]->h);
		}
	}
	float *all = NULL;
	size_t n = 0, cap = 0;
	t->nbatches = 0;
	for (int pass = 0; pass < 2; pass++) {
		x.above = pass == 1;
		for (int z = 0; z < 3 && z < m->zs; z++) {
			/* bottom row first: table legs reach into the tile below */
			for (int ty = y1; ty >= y0; ty--)
				for (int tx = x0; tx <= x1; tx++) {
					int id = m->data[wrap(tx, m->xs) + m->xs * (wrap(ty, m->ys) + m->ys * z)];
					if (id > 0)
						tile(&x, id, (tx - x0) * 32, (ty - y0) * 32);
				}
			flush_lists(t, &x, &all, &n, &cap, x.above);
			/* VX Ace: shadows (layer 4) after the second layer */
			if (z == 1 && !x.above && rgss_version >= 3 && m->zs > 3) {
				for (int ty = y0; ty <= y1; ty++)
					for (int tx = x0; tx <= x1; tx++) {
						int s = m->data[wrap(tx, m->xs) + m->xs * (wrap(ty, m->ys) + m->ys * 3)] & 0xf;
						for (int q = 0; q < 4; q++)
							if (s & (1 << q)) {
								float dx = (tx - x0) * 32 + (q & 1) * 16, dy = (ty - y0) * 32 + (q >> 1) * 16;
								push_quad(&x.shadow, dx, dy, dx + 16, dy + 16, -1, -1, -1, -1, 0.5f);
							}
					}
				flush_lists(t, &x, &all, &n, &cap, false);
			}
		}
	}
	for (int i = 0; i < 9; i++)
		free(x.lists[i].v);
	free(x.shadow.v);
	/* into linear memory for the GPU */
	if (n * sizeof(float) > t->cap) {
		gpu_free(t->verts);
		t->cap = n * sizeof(float) * 3 / 2 + 4096;
		t->verts = gpu_alloc(t->cap);
		if (!t->verts) {
			t->cap = 0;
			t->nbatches = 0;
		}
	}
	if (t->verts && n) {
		memcpy(t->verts, all, n * sizeof(float));
		gpu_flush(t->verts, n * sizeof(float));
	}
	free(all);
}

static void draw_layers(Tilemap *t, Canvas *c, int vox, int voy, bool above)
{
	if (!c->gpu || !t->visible || NIL_P(t->map_data))
		return;
	Table *m = table_get(t->map_data);
	if (m->xs <= 0 || m->ys <= 0)
		return;
	int bx = vox - t->ox, by = voy - t->oy; /* where map tile (0, 0) is */
	/* visible tiles (plus a row above: table legs) */
	int x0 = (c->clip.x - bx) >> 5, x1 = (c->clip.x + c->clip.w - 1 - bx) >> 5;
	int y0 = ((c->clip.y - by) >> 5) - 1, y1 = (c->clip.y + c->clip.h - 1 - by) >> 5;
	static const unsigned char ani_a[12] = { 0, 1, 2, 1, 0, 1, 2, 1, 0, 1, 2, 1 };
	int aa = ani_a[(t->frame / 30) % 12], ac = (t->frame / 30) % 3;
	int key[12] = { x0, y0, x1, y1, aa, ac, (int) m->version, (int) (intptr_t) m,
	                NIL_P(t->flags) ? 0 : (int) table_get(t->flags)->version, 0, 0, 0 };
	for (int i = 0; i < 9; i++) {
		Bitmap *b = bitmap_get_maybe(rb_ary_entry(t->bitmaps, i));
		key[9] = key[9] * 31 + (int) (intptr_t) b;
		key[10] = key[10] * 31 + (b ? (int) b->version : 0);
	}
	if (!t->built || memcmp(key, t->key, sizeof key)) {
		build(t, m, x0, y0, x1, y1, aa, ac);
		memcpy(t->key, key, sizeof key);
		t->built = true;
	}
	if (!t->verts)
		return;
	float tx = bx + x0 * 32, ty = by + y0 * 32;
	for (int i = 0; i < t->nbatches; i++) {
		Batch *b = &t->batches[i];
		if (b->above != above)
			continue;
		if (b->bitmap < 0) {
			gpu_untextured_begin(c);
		} else {
			Bitmap *bm = bitmap_get_maybe(rb_ary_entry(t->bitmaps, b->bitmap));
			if (!bm || !gpu_tiles_begin(c, bm, 255))
				continue;
		}
		gpu_draw_buffer(t->verts, b->first, b->count, tx, ty);
	}
}

static void tilemap_draw_ground(Element *e, Canvas *c, int ox, int oy)
{
	draw_layers((Tilemap *) e, c, ox, oy, false);
}

static void tilemap_draw_above(Element *e, Canvas *c, int ox, int oy)
{
	Tilemap *t = (Tilemap *) ((char *) e - offsetof(Tilemap, above));
	draw_layers(t, c, ox, oy, true);
}

static VALUE tilemap_dispose(VALUE self)
{
	Tilemap *t = rb_check_typeddata(self, &tilemap_type);
	if (!t->disposed) {
		t->disposed = true;
		element_unregister(&t->ground);
		element_unregister(&t->above);
	}
	return Qnil;
}
static VALUE tilemap_disposed(VALUE self) { return ((Tilemap *) rb_check_typeddata(self, &tilemap_type))->disposed ? Qtrue : Qfalse; }
static VALUE tilemap_update(VALUE self) { tilemap_get(self)->frame++; return Qnil; }
static VALUE tilemap_bitmaps(VALUE self) { return tilemap_get(self)->bitmaps; }
#define T_VAL(f) \
	static VALUE tilemap_get_##f(VALUE self) { return tilemap_get(self)->f; } \
	static VALUE tilemap_set_##f(VALUE self, VALUE v) { tilemap_get(self)->f = v; return v; }
T_VAL(map_data) T_VAL(flash_data) T_VAL(flags)
static VALUE tilemap_viewport(VALUE self) { return tilemap_get(self)->ground.viewport; }
static VALUE tilemap_set_viewport(VALUE self, VALUE v)
{
	Tilemap *t = tilemap_get(self);
	t->ground.viewport = t->above.viewport = v;
	return v;
}
static VALUE tilemap_visible(VALUE self) { return tilemap_get(self)->visible ? Qtrue : Qfalse; }
static VALUE tilemap_set_visible(VALUE self, VALUE v) { tilemap_get(self)->visible = RTEST(v); return v; }
static VALUE tilemap_ox(VALUE self) { return INT2NUM(tilemap_get(self)->ox); }
static VALUE tilemap_set_ox(VALUE self, VALUE v) { tilemap_get(self)->ox = num2int(v); return v; }
static VALUE tilemap_oy(VALUE self) { return INT2NUM(tilemap_get(self)->oy); }
static VALUE tilemap_set_oy(VALUE self, VALUE v) { tilemap_get(self)->oy = num2int(v); return v; }

void init_tilemap(void)
{
	cTilemap = rb_define_class("Tilemap", rb_cObject);
	rb_define_alloc_func(cTilemap, tilemap_alloc);
	rb_define_method(cTilemap, "initialize", tilemap_init, -1);
	rb_define_method(cTilemap, "dispose", tilemap_dispose, 0);
	rb_define_method(cTilemap, "disposed?", tilemap_disposed, 0);
	rb_define_method(cTilemap, "update", tilemap_update, 0);
	rb_define_method(cTilemap, "bitmaps", tilemap_bitmaps, 0);
	DEF_ATTR(cTilemap, "map_data", tilemap_get_map_data, tilemap_set_map_data);
	DEF_ATTR(cTilemap, "flash_data", tilemap_get_flash_data, tilemap_set_flash_data);
	DEF_ATTR(cTilemap, "flags", tilemap_get_flags, tilemap_set_flags);
	DEF_ATTR(cTilemap, "passages", tilemap_get_flags, tilemap_set_flags); /* VX */
	DEF_ATTR(cTilemap, "viewport", tilemap_viewport, tilemap_set_viewport);
	DEF_ATTR(cTilemap, "visible", tilemap_visible, tilemap_set_visible);
	DEF_ATTR(cTilemap, "ox", tilemap_ox, tilemap_set_ox);
	DEF_ATTR(cTilemap, "oy", tilemap_oy, tilemap_set_oy);
}
