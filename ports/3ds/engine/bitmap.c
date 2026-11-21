/* Bitmap: RGBA images in memory, drawn in software. */
#include "engine.h"
#include <errno.h>
#include <png.h>
#include <lz4frame.h>
#include <3ds.h>
#include <malloc.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

VALUE cBitmap;

static void bitmap_mark(void *p)
{
	Bitmap *b = p;
	rb_gc_mark(b->font);
	for (int i = 0; i < b->nops; i++) {
		rb_gc_mark(b->ops[i].str);
		rb_gc_mark(b->ops[i].font);
	}
}

/* --- sharp window text (see engine.h) */
static void layer_free(Bitmap **lp)
{
	Bitmap *l = *lp;
	if (!l)
		return;
	gpu_forget(l);
	px_free(l->px, (size_t) l->w * l->h * 4);
	free(l);
	*lp = NULL;
}

static void ops_clear(Bitmap *b)
{
	if (!b->nops)
		return;
	b->nops = 0;
	b->ops_gen++;
}

static void ops_free(Bitmap *b)
{
	free(b->ops);
	b->ops = NULL;
	b->nops = b->ops_cap = 0;
	layer_free(&b->layer);
}

/* texts in the rect are covered (fill_rect, clear_rect) */
static void ops_remove(Bitmap *b, Rect r)
{
	int n = 0;
	for (int i = 0; i < b->nops; i++) {
		Rect o = b->ops[i].r;
		bool hit = o.x < r.x + r.w && r.x < o.x + o.w && o.y < r.y + r.h && r.y < o.y + o.h;
		if (!hit)
			b->ops[n++] = b->ops[i];
	}
	if (n != b->nops) {
		b->nops = n;
		b->ops_gen++;
	}
}

/* (too many texts without a clear: into the pixels after all) */
#define MAX_OPS 2000

static bool ops_add(Bitmap *b, Rect r, VALUE str, int align)
{
	if (b->nops == b->ops_cap) {
		if (b->ops_cap >= MAX_OPS)
			return false;
		int nc = b->ops_cap ? b->ops_cap * 2 : 32;
		TextOp *n = realloc(b->ops, sizeof *n * nc);
		if (!n)
			return false;
		b->ops = n;
		b->ops_cap = nc;
	}
	/* the Ruby objects first: making them can start the garbage collector,
	 * which marks ops[0 .. nops) (bitmap_mark); counting the entry before
	 * it was filled made it mark leftover memory (a crash on the console
	 * in Changed's first scene) */
	VALUE s = rb_str_new_frozen(str);
	VALUE f = rb_obj_dup(b->font);
	b->ops[b->nops] = (TextOp) { r, align, s, f };
	b->nops++;
	RB_GC_GUARD(s);
	RB_GC_GUARD(f);
	return true;
}

static Bitmap *full_layers[16]; /* scale-1 layers made for a freeze */
static int nfull;

/* the text layer: only the recorded texts, at sx, sy (transparent elsewhere);
 * new texts are added to it, removals redraw it */
Bitmap *bitmap_text_layer(Bitmap *b, float sx, float sy)
{
	if (!b->nops)
		return NULL;
	int w = (int) (b->w * sx + 0.999f), h = (int) (b->h * sy + 0.999f);
	if (w <= 0 || h <= 0)
		return NULL;
	Bitmap *l = b->layer;
	bool redraw = !l || l->w != w || l->h != h || b->layer_gen != b->ops_gen || b->layer_sx != sx ||
	              b->layer_sy != sy || b->layer_done > b->nops;
	if (l && (l->w != w || l->h != h))
		layer_free(&b->layer), l = NULL;
	if (!l) {
		l = calloc(1, sizeof *l);
		if (!l)
			return NULL;
		l->font = Qnil;
		l->px = px_alloc_at((size_t) w * h * 4, true, true);
		if (!l->px) {
			free(l);
			return NULL;
		}
		l->w = w;
		l->h = h;
		b->layer = l;
	}
	int from = b->layer_done;
	if (redraw) {
		memset(l->px, 0, (size_t) w * h * 4);
		from = 0;
	}
	if (from == b->nops && !redraw)
		return l;
	for (int i = from; i < b->nops; i++) {
		TextOp *o = &b->ops[i];
		int x0 = (int) (o->r.x * sx + 0.5f), y0 = (int) (o->r.y * sy + 0.5f);
		int x1 = (int) ((o->r.x + o->r.w) * sx + 0.5f), y1 = (int) ((o->r.y + o->r.h) * sy + 0.5f);
		/* (letters twice as wide for the wide top screen: sx is twice sy) */
		int xs = sx >= 1.9f * sy ? 2 : 1;
		font_draw_text_scaled(l, o->font, x0, y0, x1 - x0, y1 - y0, o->str, o->align, sy, xs);
	}
	l->version++;
	b->layer_gen = b->ops_gen;
	b->layer_done = b->nops;
	b->layer_sx = sx;
	b->layer_sy = sy;
	if (sx == 1 && sy == 1 && nfull < 16) /* (only for a freeze: let go after it) */
		full_layers[nfull++] = b;
	return l;
}

void bitmap_text_layers_drop_full(void)
{
	for (int i = 0; i < nfull; i++)
		if (full_layers[i]->layer && full_layers[i]->layer_sx == 1 && full_layers[i]->layer_sy == 1)
			layer_free(&full_layers[i]->layer);
	nfull = 0;
}

static void drop_from_full(Bitmap *b)
{
	for (int i = 0; i < nfull; i++)
		if (full_layers[i] == b)
			full_layers[i--] = full_layers[--nfull];
}
/* --- dropping pixels of images loaded from files (the game's cache keeps
 * every image it loaded; the original 3DS has only ~25 MB of normal memory
 * for a game). The list: file images that have pixels, most recently used
 * first. */
static Bitmap *lru_first, *lru_last;
unsigned bitmap_drops, bitmap_reloads;
#define HEAP_RESERVE (4 * 1024 * 1024) /* kept free for Ruby */

static void lru_remove(Bitmap *b)
{
	if (b->lru_prev) b->lru_prev->lru_next = b->lru_next; else if (lru_first == b) lru_first = b->lru_next;
	if (b->lru_next) b->lru_next->lru_prev = b->lru_prev; else if (lru_last == b) lru_last = b->lru_prev;
	b->lru_prev = b->lru_next = NULL;
}

static void lru_touch(Bitmap *b)
{
	if (!b->file || !b->px || lru_first == b)
		return;
	lru_remove(b);
	b->lru_next = lru_first;
	if (lru_first)
		lru_first->lru_prev = b;
	lru_first = b;
	if (!lru_last)
		lru_last = b;
}

/* lets go of the pixels of the image used longest ago (unchanged since
 * loaded; the 4 most recently used are kept: they may be in use right
 * now); in_arena: only those in the pixel arena (memory.c), else only
 * those in the normal heap. false: nothing left to let go of */
static bool drop_one_keeping(bool in_arena, int keep)
{
	for (Bitmap *b = lru_last; b; b = b->lru_prev) {
		int n = 0;
		for (Bitmap *x = b->lru_prev; x && n < keep; x = x->lru_prev)
			n++;
		if (n < keep)
			return false;
		if (b->version != b->file_version || px_in_arena(b->px) != in_arena)
			continue;
		lru_remove(b);
		px_free(b->px, (size_t) b->w * b->h * 4);
		b->px = NULL;
		b->dropped = true;
		bitmap_drops++;
		return true;
	}
	return false;
}

/* gpu.c: the texture was just made. An image of 256 KB or more loaded from a file and
 * unchanged lets go of its pixels right away: the graphics chip has them
 * (a 1536x1024 character sheet is 6 MB; the arena once had 8.8 MB free in
 * pieces and no 6 MB block for the next one). They're read again from the
 * file when needed (texture evicted, or a script reads pixels). */
unsigned bitmap_released;
void bitmap_uploaded(Bitmap *b)
{
	if (!b->file || !b->px || b->version != b->file_version || (size_t) b->w * b->h * 4 < 256 * 1024)
		return;
	lru_remove(b);
	px_free(b->px, (size_t) b->w * b->h * 4);
	b->px = NULL;
	b->dropped = true;
	bitmap_released++;
}

static bool drop_one(bool in_arena)
{
	return drop_one_keeping(in_arena, 4);
}

/* memory.c: a pixel buffer (or fiber stack) didn't fit. The 4 most
 * recently used images are kept first; if that's not enough (a 1536x1024
 * character sheet is 6 MB in one piece), down to the last one used: no
 * more than that is in use while a new buffer is made. */
static int keep_at_least = 1; /* 0 while Bitmap.new loads a file */
bool mem_release(size_t need, bool in_arena)
{
	return drop_one_keeping(in_arena, 4) || drop_one_keeping(in_arena, keep_at_least);
}

/* keeps the normal heap's reserve free (for Ruby); need: a pixel buffer
 * that goes there (small ones; big ones go into the arena) */
static void make_room(size_t need)
{
	if (need >= PX_MAP_MIN)
		need = 0;
	while (heap_free() < need + HEAP_RESERVE && drop_one(false))
		;
}

static bool load_image(Bitmap *b, const char *path);

static Bitmap *reload_target;
static VALUE reload_protected(VALUE path)
{
	return load_image(reload_target, (const char *) path) ? Qtrue : Qfalse;
}

px_t *bitmap_px(Bitmap *b)
{
	if (!b->px && b->dropped && !b->disposed) {
		int w = b->w, h = b->h;
		/* (read again while drawing: the last image used may be in use) */
		keep_at_least = 1;
		make_room((size_t) w * h * 4);
		Bitmap t = { 0 };
		/* (no Ruby error from here: it may be in the middle of drawing) */
		int state = 0;
		reload_target = &t;
		VALUE ok = rb_protect(reload_protected, (VALUE) b->file, &state);
		if (state)
			rb_set_errinfo(Qnil);
		if (!state && ok == Qtrue && t.w == w && t.h == h) {
			b->px = t.px;
			b->dropped = false;
			bitmap_reloads++;
		} else {
			px_free(t.px, (size_t) t.w * t.h * 4);
			log_only("%s: couldn't read it again\n", b->file);
		}
	}
	if (b->px)
		lru_touch(b);
	return b->px;
}

static void bitmap_forget_file(Bitmap *b)
{
	lru_remove(b);
	free(b->file);
	b->file = NULL;
	b->dropped = false;
}

static void bitmap_free(void *p)
{
	Bitmap *b = p;
	gpu_forget(b);
	bitmap_forget_file(b);
	px_free(b->px, (size_t) b->w * b->h * 4);
	drop_from_full(b);
	gpu_text_forget(b);
	ops_free(b); /* (its strings and fonts are Ruby's) */
	xfree(b);
}
static size_t bitmap_size(const void *p)
{
	const Bitmap *b = p;
	return sizeof *b + (size_t) b->w * b->h * 4;
}
static const rb_data_type_t bitmap_type = { "Bitmap", { bitmap_mark, bitmap_free, bitmap_size }, 0, 0, 0 };

static VALUE bitmap_alloc(VALUE klass)
{
	Bitmap *b;
	VALUE v = TypedData_Make_Struct(klass, Bitmap, &bitmap_type, b);
	b->font = Qnil;
	return v;
}

Bitmap *bitmap_get_maybe(VALUE v)
{
	if (NIL_P(v))
		return NULL;
	Bitmap *b = rb_check_typeddata(v, &bitmap_type);
	return b->disposed || (!b->px && !b->dropped) ? NULL : b;
}

Bitmap *bitmap_get(VALUE v)
{
	Bitmap *b = rb_check_typeddata(v, &bitmap_type);
	if (b->disposed)
		rb_raise(eRGSSError, "disposed bitmap");
	if (!bitmap_px(b)) {
		if (b->dropped) {
			rb_gc();
			if (bitmap_px(b))
				return b;
			rb_raise(rb_eNoMemError, "out of memory reading %s again", b->file);
		}
		rb_raise(eRGSSError, "uninitialized bitmap");
	}
	return b;
}

static bool loading_file; /* (bitmap_set_size: for a file image) */

static void bitmap_set_size(Bitmap *b, int w, int h)
{
	if (w <= 0 || h <= 0)
		rb_raise(rb_eArgError, "failed to create bitmap (%dx%d)", w, h);
	make_room((size_t) w * h * 4);
	b->px = px_alloc_at((size_t) w * h * 4, true, !loading_file);
	if (!b->px) {
		rb_gc();
		b->px = px_alloc_at((size_t) w * h * 4, true, !loading_file);
		if (!b->px) {
			log_only("no memory for a %dx%d bitmap: %lu KB free, %u images in the arena (%lu KB, %lu KB free)\n", w, h,
			         (unsigned long) (heap_free() / 1024), mem_px_maps, (unsigned long) (mem_px_bytes / 1024),
			         (unsigned long) (mem_arena_free() / 1024));
			rb_raise(rb_eNoMemError, "failed to create bitmap (%dx%d): out of memory", w, h);
		}
	}
	b->w = w;
	b->h = h;
}

/* --- loading images: PNG (with or without extension) */
static bool load_png(Bitmap *b, const char *path)
{
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path))
		return false;
	loading_tick(-1);
	img.format = PNG_FORMAT_RGBA;
	bitmap_set_size(b, img.width, img.height);
	bool ok = png_image_finish_read(&img, NULL, b->px, 0, NULL);
	if (!ok) {
		png_image_free(&img);
		px_free(b->px, (size_t) b->w * b->h * 4);
		b->px = NULL;
		return false;
	}
	return true;
}

/* RGSS finds "Graphics/Titles1/Foo" as Foo.png (also case-insensitively on
 * the SD card, whose file system ignores case anyway) */
static bool find_image(const char *name, char *out, size_t n)
{
	/* converted ahead (.mkxi) first: also for a name given with .png */
	char stem[512];
	snprintf(stem, sizeof stem, "%s", name);
	char *dot = strrchr(stem, '.');
	if (dot && !strchr(dot, '/') && !strchr(dot, '\\') &&
	    (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".bmp")))
		*dot = 0;
	static const char *const mkxi[] = { ".mkxi", NULL };
	if (file_find(stem, mkxi, out, n))
		return true;
	static const char *const ext[] = { ".png", ".jpg", ".bmp", "", NULL };
	return file_find(name, ext, out, n);
}

/* .mkxi (made by tools/convert-images.py): "MKXI", version, width, height
 * (32 bits each), then the RGBA pixels as an LZ4 frame; unpacks far faster
 * than PNG */
static bool load_mkxi(Bitmap *b, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size > 16)
		make_room((size_t) size);
	unsigned char *data = size > 16 ? px_alloc(size, false) : NULL;
	size_t got = data ? fread(data, 1, size, f) : 0;
	bool ok = data && got == (size_t) size;
	fclose(f);
	if (!ok)
		log_only("%s: read %u of %ld bytes%s\n", path, (unsigned) got, size, data ? "" : " (no memory)");
	uint32_t head[4];
	if (ok)
		memcpy(head, data, 16);
	if (!ok || memcmp(head, "MKXI", 4) || head[1] != 1 || !head[2] || !head[3] || head[2] > 8192 || head[3] > 8192) {
		px_free(data, size > 16 ? size : 0);
		return false;
	}
	bitmap_set_size(b, head[2], head[3]);
	LZ4F_dctx *dctx;
	if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) {
		log_only("%s: no LZ4 context\n", path);
		px_free(data, size);
		return false;
	}
	size_t want = (size_t) b->w * b->h * 4, done = 0, pos = 16;
	while (done < want && pos < (size_t) size) {
		size_t dst = want - done, src = size - pos;
		if (dst > 256 * 1024)
			dst = 256 * 1024; /* (in steps: the loading bar) */
		loading_tick((float) done / want);
		size_t r = LZ4F_decompress(dctx, (char *) b->px + done, &dst, data + pos, &src, NULL);
		if (LZ4F_isError(r)) {
			log_only("%s: %s\n", path, LZ4F_getErrorName(r));
			break;
		}
		done += dst;
		pos += src;
		if (r == 0)
			break;
	}
	LZ4F_freeDecompressionContext(dctx);
	px_free(data, size);
	if (done != want) {
		log_only("%s: unpacked %u of %u bytes\n", path, (unsigned) done, (unsigned) want);
		px_free(b->px, want);
		b->px = NULL;
		return false;
	}
	return true;
}

/* the pixels of an image that let go of them, from its .mkxi file in blocks
 * of 32 rows: textures are made from them without the whole image in memory
 * (a 1536x1024 character sheet is 6 MB in one piece; the console once had
 * 8 MB free in the arena, but no 6 MB block) */
bool bitmap_stream_file(Bitmap *b, void (*rows)(void *ctx, int y0, int n, const px_t *px), void *ctx)
{
	if (!b->file || b->version != b->file_version)
		return false;
	size_t l = strlen(b->file);
	if (l < 5 || strcasecmp(b->file + l - 5, ".mkxi"))
		return false;
	FILE *f = fopen(b->file, "rb");
	if (!f)
		return false;
	uint32_t head[4];
	if (fread(head, 1, 16, f) != 16 || memcmp(head, "MKXI", 4) || head[1] != 1 || (int) head[2] != b->w ||
	    (int) head[3] != b->h) {
		fclose(f);
		return false;
	}
	size_t rowbytes = (size_t) b->w * 4, cap = rowbytes * 32, have = 0;
	unsigned char *buf = malloc(cap), *in = malloc(16384);
	LZ4F_dctx *dctx = NULL;
	bool ok = buf && in && !LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION));
	int y = 0;
	size_t n = 0, pos = 0;
	bool eof = false;
	while (ok) {
		if (pos == n && !eof) {
			n = fread(in, 1, 16384, f);
			pos = 0;
			eof = n == 0;
		}
		size_t dst = cap - have, src = n - pos;
		size_t r = LZ4F_decompress(dctx, buf + have, &dst, in + pos, &src, NULL);
		if (LZ4F_isError(r)) {
			log_only("%s: %s\n", b->file, LZ4F_getErrorName(r));
			ok = false;
			break;
		}
		have += dst;
		pos += src;
		int nrows = (int) (have / rowbytes);
		if (nrows > b->h - y)
			nrows = b->h - y;
		if (nrows > 0) {
			loading_tick((float) y / b->h);
			rows(ctx, y, nrows, (const px_t *) buf);
			y += nrows;
			memmove(buf, buf + (size_t) nrows * rowbytes, have - (size_t) nrows * rowbytes);
			have -= (size_t) nrows * rowbytes;
		}
		if (r == 0 || y >= b->h)
			break;
		if (eof && !dst && !src)
			ok = false; /* (cut off) */
	}
	if (dctx)
		LZ4F_freeDecompressionContext(dctx);
	free(buf);
	free(in);
	fclose(f);
	return ok && y == b->h;
}

/* (texture uploads: only this image's pixels are in use) */
px_t *bitmap_px_alone(Bitmap *b)
{
	if (b->px || !b->dropped)
		return bitmap_px(b);
	int was = keep_at_least;
	keep_at_least = 0;
	px_t *p = bitmap_px(b);
	keep_at_least = was;
	return p;
}

static bool load_image_inner(Bitmap *b, const char *path);
static bool load_image(Bitmap *b, const char *path)
{
	bool was = loading_file;
	loading_file = true;
	bool ok = load_image_inner(b, path);
	loading_file = was;
	return ok;
}

static bool load_image_inner(Bitmap *b, const char *path)
{
	size_t l = strlen(path);
	if (l > 5 && !strcasecmp(path + l - 5, ".mkxi"))
		return load_mkxi(b, path);
	return load_png(b, path);
}

unsigned bitmaps_created;

static VALUE bitmap_init(int argc, VALUE *argv, VALUE self)
{
	Bitmap *b = rb_check_typeddata(self, &bitmap_type);
	/* (the log: how many, and every 50th one's script line) */
	static unsigned noted;
	if (++bitmaps_created % 50 == 0 && noted < 40) {
		char where[600];
		prof_caller(where, sizeof where);
		noted++;
		log_only("Bitmap.new (%s) from %s\n", argc == 1 ? "file" : "size", where);
	}
	if (argc == 1) {
		const char *name = StringValueCStr(argv[0]);
		char path[512];
		/* (nothing else's pixels are in use while Bitmap.new loads a file:
		 * every other file image may let go of its pixels to make room) */
		keep_at_least = 0;
		bool found = find_image(name, path, sizeof path), loaded = false;
		/* a big .mkxi: only its size now; the pixels when needed (textures
		 * straight from the file: bitmap_stream_file) */
		size_t pl = found ? strlen(path) : 0;
		if (pl > 5 && !strcasecmp(path + pl - 5, ".mkxi")) {
			FILE *hf = fopen(path, "rb");
			uint32_t head[4];
			if (hf && fread(head, 1, 16, hf) == 16 && !memcmp(head, "MKXI", 4) && head[1] == 1 && head[2] &&
			    head[3] && head[2] <= 8192 && head[3] <= 8192 && (size_t) head[2] * head[3] * 4 >= 256 * 1024) {
				b->w = (int) head[2];
				b->h = (int) head[3];
				b->dropped = true;
				loaded = true;
			}
			if (hf)
				fclose(hf);
		}
		if (found && !loaded)
			loaded = load_image(b, path);
		keep_at_least = 1;
		if (found && !loaded) {
			/* a .mkxi that doesn't load: the PNG */
			size_t l = strlen(path);
			if (l > 5 && !strcasecmp(path + l - 5, ".mkxi")) {
				static const char *const exts[] = { ".png", ".jpg", ".bmp", NULL };
				char alt[512] = "";
				if (file_find(name, exts, alt, sizeof alt) && (loaded = load_image(b, alt)))
					snprintf(path, sizeof path, "%s", alt);
				log_only("%s: PNG %s\n", name, loaded ? "loaded instead" : "not found or not loadable either");
			} else {
				log_only("%s: couldn't load\n", path);
			}
		}
		if (loaded) {
			b->file = strdup(path);
			b->file_version = b->version;
			lru_touch(b);
		}
		if (!loaded) {
			/* missing (e.g. from the RTP, which isn't on the 3DS): an empty
			 * image instead of stopping the game; noted once per name */
			static char noted[16][128];
			static int nnoted;
			bool seen = false;
			for (int i = 0; i < nnoted && !seen; i++)
				seen = !strcmp(noted[i], name);
			if (!seen) {
				say("Missing image: %s\n", name);
				if (nnoted < 16)
					snprintf(noted[nnoted++], sizeof noted[0], "%s", name);
			}
			bitmap_set_size(b, 32, 32);
		}
	} else if (argc == 2) {
		bitmap_set_size(b, num2int(argv[0]), num2int(argv[1]));
		/* (the log: big ones the game draws into, with the script lines) */
		static unsigned big_noted;
		if ((size_t) b->w * b->h * 4 >= 256 * 1024 && big_noted < 30) {
			char where[600];
			prof_caller(where, sizeof where);
			big_noted++;
			log_only("big Bitmap.new(%d, %d) from %s\n", b->w, b->h, where);
		}
	} else {
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 1 or 2)", argc);
	}
	b->font = rb_class_new_instance(0, NULL, cFont);
	mem_tell_ruby();
	return self;
}

static VALUE bitmap_init_copy(VALUE self, VALUE orig)
{
	Bitmap *b = rb_check_typeddata(self, &bitmap_type), *o = bitmap_get(orig);
	bitmap_set_size(b, o->w, o->h);
	memcpy(b->px, o->px, (size_t) o->w * o->h * 4);
	b->font = rb_obj_dup(o->font);
	/* (a copy gets the texts drawn into its pixels) */
	for (int i = 0; i < o->nops; i++)
		font_draw_text(b, o->ops[i].font, o->ops[i].r.x, o->ops[i].r.y, o->ops[i].r.w, o->ops[i].r.h, o->ops[i].str,
		               o->ops[i].align);
	return self;
}

VALUE bitmap_new_size(int w, int h)
{
	VALUE a[2] = { INT2NUM(w), INT2NUM(h) };
	return rb_class_new_instance(2, a, cBitmap);
}

static VALUE bitmap_dispose(VALUE self)
{
	Bitmap *b = rb_check_typeddata(self, &bitmap_type);
	b->disposed = true;
	gpu_forget(b);
	bitmap_forget_file(b);
	px_free(b->px, (size_t) b->w * b->h * 4);
	b->px = NULL;
	drop_from_full(b);
	gpu_text_forget(b);
	ops_free(b);
	mem_tell_ruby();
	return Qnil;
}
static VALUE bitmap_disposed(VALUE self) { return ((Bitmap *) rb_check_typeddata(self, &bitmap_type))->disposed ? Qtrue : Qfalse; }
/* the size: also without pixels (an image that let go of them isn't read
 * again for this; Sprite_Character asks every character sheet) */
static Bitmap *bitmap_get_size(VALUE self)
{
	Bitmap *b = rb_check_typeddata(self, &bitmap_type);
	if (b->disposed)
		rb_raise(eRGSSError, "disposed bitmap");
	if (!b->px && !b->dropped)
		rb_raise(eRGSSError, "uninitialized bitmap");
	return b;
}
static VALUE bitmap_width(VALUE self) { return INT2NUM(bitmap_get_size(self)->w); }
static VALUE bitmap_height(VALUE self) { return INT2NUM(bitmap_get_size(self)->h); }
static VALUE bitmap_rect(VALUE self) { Bitmap *b = bitmap_get_size(self); return rect_new(0, 0, b->w, b->h); }

/* --- pixel helpers */
/* "over" compositing with straight alpha; a: source alpha 0..255 */
static inline px_t over(px_t d, px_t s, int a)
{
	if (a <= 0)
		return d;
	int da = PX_A(d);
	if (a >= 255 || da == 0)
		return (s & 0x00ffffff) | ((px_t) a << 24);
	int oa = a + da * (255 - a) / 255;
	int r = (PX_R(s) * a + PX_R(d) * da * (255 - a) / 255) / oa;
	int g = (PX_G(s) * a + PX_G(d) * da * (255 - a) / 255) / oa;
	int bl = (PX_B(s) * a + PX_B(d) * da * (255 - a) / 255) / oa;
	return PX(r, g, bl, oa);
}

static bool clip(Bitmap *dst, int *dx, int *dy, Rect *sr, int sw, int sh)
{
	/* source rect inside the source */
	if (sr->x < 0) { *dx -= sr->x; sr->w += sr->x; sr->x = 0; }
	if (sr->y < 0) { *dy -= sr->y; sr->h += sr->y; sr->y = 0; }
	if (sr->x + sr->w > sw) sr->w = sw - sr->x;
	if (sr->y + sr->h > sh) sr->h = sh - sr->y;
	/* and its destination inside the destination */
	if (*dx < 0) { sr->x -= *dx; sr->w += *dx; *dx = 0; }
	if (*dy < 0) { sr->y -= *dy; sr->h += *dy; *dy = 0; }
	if (*dx + sr->w > dst->w) sr->w = dst->w - *dx;
	if (*dy + sr->h > dst->h) sr->h = dst->h - *dy;
	return sr->w > 0 && sr->h > 0;
}

void bitmap_blt(Bitmap *dst, int dx, int dy, Bitmap *src, Rect sr, int opacity)
{
	if (opacity <= 0 || !clip(dst, &dx, &dy, &sr, src->w, src->h))
		return;
	for (int y = 0; y < sr.h; y++) {
		const px_t *s = src->px + (size_t) (sr.y + y) * src->w + sr.x;
		px_t *d = dst->px + (size_t) (dy + y) * dst->w + dx;
		for (int x = 0; x < sr.w; x++) {
			int a = PX_A(s[x]) * opacity / 255;
			d[x] = over(d[x], s[x], a);
		}
	}
	dst->version++;
}

static Rect rect_arg(VALUE v) { return *rect_get(v); }

static VALUE bitmap_blt_m(int argc, VALUE *argv, VALUE self)
{
	if (argc < 4 || argc > 5)
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 4..5)", argc);
	Bitmap *d = bitmap_get(self), *s = bitmap_get(argv[2]);
	int op = argc == 5 ? CLAMP(num2int(argv[4]), 0, 255) : 255;
	bitmap_blt(d, num2int(argv[0]), num2int(argv[1]), s, rect_arg(argv[3]), op);
	return self;
}

static VALUE bitmap_stretch_blt(int argc, VALUE *argv, VALUE self)
{
	if (argc < 3 || argc > 4)
		rb_raise(rb_eArgError, "wrong number of arguments (%d for 3..4)", argc);
	Bitmap *d = bitmap_get(self), *s = bitmap_get(argv[1]);
	Rect dr = rect_arg(argv[0]), sr = rect_arg(argv[2]);
	int op = argc == 4 ? CLAMP(num2int(argv[3]), 0, 255) : 255;
	if (dr.w <= 0 || dr.h <= 0 || sr.w == 0 || sr.h == 0 || op == 0)
		return self;
	for (int y = 0; y < dr.h; y++) {
		int ty = dr.y + y;
		if (ty < 0 || ty >= d->h)
			continue;
		int sy = sr.y + (int) ((long long) y * sr.h / dr.h);
		if (sy < 0 || sy >= s->h)
			continue;
		px_t *dp = d->px + (size_t) ty * d->w;
		const px_t *sp = s->px + (size_t) sy * s->w;
		for (int x = 0; x < dr.w; x++) {
			int tx = dr.x + x;
			int sx = sr.x + (int) ((long long) x * sr.w / dr.w);
			if (tx < 0 || tx >= d->w || sx < 0 || sx >= s->w)
				continue;
			dp[tx] = over(dp[tx], sp[sx], PX_A(sp[sx]) * op / 255);
		}
	}
	d->version++;
	return self;
}

static void fill(Bitmap *b, Rect r, px_t p)
{
	if (b->nops)
		ops_remove(b, r);
	if (r.x < 0) { r.w += r.x; r.x = 0; }
	if (r.y < 0) { r.h += r.y; r.y = 0; }
	if (r.x + r.w > b->w) r.w = b->w - r.x;
	if (r.y + r.h > b->h) r.h = b->h - r.y;
	if (r.w <= 0 || r.h <= 0)
		return;
	for (int y = 0; y < r.h; y++) {
		px_t *d = b->px + (size_t) (r.y + y) * b->w + r.x;
		for (int x = 0; x < r.w; x++)
			d[x] = p;
	}
	b->version++;
}

static px_t color_px(VALUE c)
{
	Color *co = color_get(c);
	return PX((int) co->r, (int) co->g, (int) co->b, (int) co->a);
}

/* (x, y, w, h, ...) or (rect, ...): the rect, and where the rest starts */
static Rect rect_or_xywh(int argc, VALUE *argv, int *rest)
{
	if (argc >= 1 && rb_obj_is_kind_of(argv[0], cRect)) {
		*rest = 1;
		return rect_arg(argv[0]);
	}
	if (argc < 4)
		rb_raise(rb_eArgError, "wrong number of arguments");
	*rest = 4;
	return (Rect) { num2int(argv[0]), num2int(argv[1]), num2int(argv[2]), num2int(argv[3]) };
}

static VALUE bitmap_fill_rect(int argc, VALUE *argv, VALUE self)
{
	int rest;
	Rect r = rect_or_xywh(argc, argv, &rest);
	if (argc <= rest)
		rb_raise(rb_eArgError, "wrong number of arguments");
	fill(bitmap_get(self), r, color_px(argv[rest]));
	return self;
}

static VALUE bitmap_gradient_fill_rect(int argc, VALUE *argv, VALUE self)
{
	int rest;
	Rect r = rect_or_xywh(argc, argv, &rest);
	if (argc < rest + 2)
		rb_raise(rb_eArgError, "wrong number of arguments");
	Bitmap *b = bitmap_get(self);
	Color c1 = *color_get(argv[rest]), c2 = *color_get(argv[rest + 1]);
	bool vertical = argc > rest + 2 && RTEST(argv[rest + 2]);
	int n = vertical ? r.h : r.w;
	for (int i = 0; i < n; i++) {
		double t = n > 1 ? (double) i / (n - 1) : 0;
		px_t p = PX((int) (c1.r + (c2.r - c1.r) * t), (int) (c1.g + (c2.g - c1.g) * t),
		            (int) (c1.b + (c2.b - c1.b) * t), (int) (c1.a + (c2.a - c1.a) * t));
		fill(b, vertical ? (Rect) { r.x, r.y + i, r.w, 1 } : (Rect) { r.x + i, r.y, 1, r.h }, p);
	}
	return self;
}

static VALUE bitmap_clear(VALUE self)
{
	Bitmap *b = bitmap_get(self);
	memset(b->px, 0, (size_t) b->w * b->h * 4);
	ops_clear(b);
	b->version++;
	return self;
}

static VALUE bitmap_clear_rect(int argc, VALUE *argv, VALUE self)
{
	int rest;
	Rect r = rect_or_xywh(argc, argv, &rest);
	fill(bitmap_get(self), r, 0);
	return self;
}

static VALUE bitmap_get_pixel(VALUE self, VALUE vx, VALUE vy)
{
	Bitmap *b = bitmap_get(self);
	int x = num2int(vx), y = num2int(vy);
	if (x < 0 || y < 0 || x >= b->w || y >= b->h)
		return color_new(0, 0, 0, 0);
	px_t p = b->px[(size_t) y * b->w + x];
	return color_new(PX_R(p), PX_G(p), PX_B(p), PX_A(p));
}

static VALUE bitmap_set_pixel(VALUE self, VALUE vx, VALUE vy, VALUE c)
{
	Bitmap *b = bitmap_get(self);
	int x = num2int(vx), y = num2int(vy);
	if (x >= 0 && y >= 0 && x < b->w && y < b->h) {
		b->px[(size_t) y * b->w + x] = color_px(c);
		b->version++;
	}
	return self;
}

static VALUE bitmap_hue_change(VALUE self, VALUE vh)
{
	Bitmap *b = bitmap_get(self);
	double h = fmod(num2dbl(vh), 360.0) * M_PI / 180.0;
	if (h == 0)
		return self;
	/* rotation around the gray axis */
	double c = cos(h), s = sin(h), k = (1 - c) / 3, q = sqrt(1.0 / 3) * s;
	double m[3][3] = { { c + k, k - q, k + q }, { k + q, c + k, k - q }, { k - q, k + q, c + k } };
	size_t n = (size_t) b->w * b->h;
	for (size_t i = 0; i < n; i++) {
		px_t p = b->px[i];
		if (!PX_A(p))
			continue;
		double r = PX_R(p), g = PX_G(p), bl = PX_B(p);
		int nr = (int) (m[0][0] * r + m[0][1] * g + m[0][2] * bl);
		int ng = (int) (m[1][0] * r + m[1][1] * g + m[1][2] * bl);
		int nb = (int) (m[2][0] * r + m[2][1] * g + m[2][2] * bl);
		b->px[i] = PX(CLAMP(nr, 0, 255), CLAMP(ng, 0, 255), CLAMP(nb, 0, 255), PX_A(p));
	}
	b->version++;
	return self;
}

/* simple 3x3 box blur (RGSS2+) */
static VALUE bitmap_blur(VALUE self)
{
	Bitmap *b = bitmap_get(self);
	px_t *o = malloc((size_t) b->w * b->h * 4);
	if (!o)
		return self;
	memcpy(o, b->px, (size_t) b->w * b->h * 4);
	for (int y = 0; y < b->h; y++)
		for (int x = 0; x < b->w; x++) {
			int s[4] = {0}, n = 0;
			for (int j = -1; j <= 1; j++)
				for (int i = -1; i <= 1; i++) {
					int xx = x + i, yy = y + j;
					if (xx < 0 || yy < 0 || xx >= b->w || yy >= b->h)
						continue;
					px_t p = o[(size_t) yy * b->w + xx];
					s[0] += PX_R(p); s[1] += PX_G(p); s[2] += PX_B(p); s[3] += PX_A(p);
					n++;
				}
			b->px[(size_t) y * b->w + x] = PX(s[0] / n, s[1] / n, s[2] / n, s[3] / n);
		}
	free(o);
	b->version++;
	return self;
}
static VALUE bitmap_radial_blur(VALUE self, VALUE a, VALUE d) { return bitmap_blur(self); }

static VALUE bitmap_font(VALUE self) { return bitmap_get(self)->font; }
static VALUE bitmap_set_font(VALUE self, VALUE f) { bitmap_get(self)->font = f; return f; }

static VALUE bitmap_draw_text(int argc, VALUE *argv, VALUE self)
{
	Bitmap *b = bitmap_get(self);
	int rest;
	Rect r = rect_or_xywh(argc, argv, &rest);
	if (argc <= rest)
		rb_raise(rb_eArgError, "wrong number of arguments");
	VALUE str = argv[rest];
	if (!RB_TYPE_P(str, T_STRING))
		str = rb_obj_as_string(str);
	int align = argc > rest + 1 ? num2int(argv[rest + 1]) : 0;
	if (b->text_native && settings.sharp_text && RSTRING_LEN(str) > 0 && ops_add(b, r, str, align))
		return self; /* (drawn sharp on the finished picture; gpu.c) */
	font_draw_text(b, b->font, r.x, r.y, r.w, r.h, str, align);
	b->version++;
	return self;
}

static VALUE bitmap_text_size(VALUE self, VALUE str)
{
	Bitmap *b = bitmap_get(self);
	if (!RB_TYPE_P(str, T_STRING))
		str = rb_obj_as_string(str);
	int w, h;
	font_text_size(b->font, str, &w, &h);
	return rect_new(0, 0, w, h);
}

static VALUE bitmap_tex_info(VALUE self)
{
	char s[200];
	void gpu_tex_info(Bitmap *b, char *out, size_t n);
	gpu_tex_info((Bitmap *) rb_check_typeddata(self, &bitmap_type), s, sizeof s);
	return rb_str_new_cstr(s);
}

void init_bitmap(void)
{
	cBitmap = rb_define_class("Bitmap", rb_cObject);
	rb_define_alloc_func(cBitmap, bitmap_alloc);
	rb_define_method(cBitmap, "initialize", bitmap_init, -1);
	rb_define_method(cBitmap, "initialize_copy", bitmap_init_copy, 1);
	rb_define_method(cBitmap, "dispose", bitmap_dispose, 0);
	rb_define_method(cBitmap, "disposed?", bitmap_disposed, 0);
	rb_define_method(cBitmap, "mkxpz_tex_info", bitmap_tex_info, 0);
	rb_define_method(cBitmap, "width", bitmap_width, 0);
	rb_define_method(cBitmap, "height", bitmap_height, 0);
	rb_define_method(cBitmap, "rect", bitmap_rect, 0);
	rb_define_method(cBitmap, "blt", bitmap_blt_m, -1);
	rb_define_method(cBitmap, "stretch_blt", bitmap_stretch_blt, -1);
	rb_define_method(cBitmap, "fill_rect", bitmap_fill_rect, -1);
	rb_define_method(cBitmap, "gradient_fill_rect", bitmap_gradient_fill_rect, -1);
	rb_define_method(cBitmap, "clear", bitmap_clear, 0);
	rb_define_method(cBitmap, "clear_rect", bitmap_clear_rect, -1);
	rb_define_method(cBitmap, "get_pixel", bitmap_get_pixel, 2);
	rb_define_method(cBitmap, "set_pixel", bitmap_set_pixel, 3);
	rb_define_method(cBitmap, "hue_change", bitmap_hue_change, 1);
	rb_define_method(cBitmap, "blur", bitmap_blur, 0);
	rb_define_method(cBitmap, "radial_blur", bitmap_radial_blur, 2);
	rb_define_method(cBitmap, "draw_text", bitmap_draw_text, -1);
	rb_define_method(cBitmap, "text_size", bitmap_text_size, 1);
	DEF_ATTR(cBitmap, "font", bitmap_font, bitmap_set_font);
}
