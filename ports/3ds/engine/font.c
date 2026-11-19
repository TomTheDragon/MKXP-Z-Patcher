/* Text with FreeType. The Font class itself is plain Ruby (boot.rb); this
 * reads its attributes when drawing. Font files: the game's Fonts folder
 * and the engine's own fonts, found by family name. */
#include "engine.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

VALUE cFont;

typedef struct {
	char family[64];
	char path[256];
	FT_Face face; /* opened when first used */
} FontFile;

static FT_Library ft;
static FontFile files[32];
static int nfiles;

static void scan_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) && nfiles < (int) (sizeof files / sizeof *files)) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf") && strcasecmp(dot, ".ttc")))
			continue;
		FontFile *f = &files[nfiles];
		snprintf(f->path, sizeof f->path, "%s/%s", dir, e->d_name);
		FT_Face face;
		if (FT_New_Face(ft, f->path, 0, &face))
			continue;
		snprintf(f->family, sizeof f->family, "%s", face->family_name ? face->family_name : "");
		FT_Done_Face(face);
		nfiles++;
	}
	closedir(d);
}

/* the engine's own fonts first (the fallback is the first one), then the game's */
void fonts_scan(const char *engine_dir)
{
	if (!ft && FT_Init_FreeType(&ft))
		return;
	scan_dir(engine_dir);
	scan_dir("Fonts");
}

static FontFile *find_family(const char *name)
{
	for (int i = 0; i < nfiles; i++)
		if (!strcasecmp(files[i].family, name))
			return &files[i];
	return NULL;
}

static VALUE font_exist(VALUE klass, VALUE name)
{
	return find_family(StringValueCStr(name)) ? Qtrue : Qfalse;
}

/* the first available of the font's name(s), else the fallback */
static FT_Face face_for(VALUE font)
{
	VALUE name = rb_ivar_get(font, rb_intern("@name"));
	FontFile *f = NULL;
	if (RB_TYPE_P(name, T_ARRAY)) {
		for (long i = 0; i < RARRAY_LEN(name) && !f; i++)
			if (RB_TYPE_P(RARRAY_AREF(name, i), T_STRING))
				f = find_family(StringValueCStr(RARRAY_AREF(name, i)));
	} else if (RB_TYPE_P(name, T_STRING)) {
		f = find_family(StringValueCStr(name));
	}
	if (!f && nfiles > 0)
		f = &files[0];
	if (!f)
		return NULL;
	if (!f->face && FT_New_Face(ft, f->path, 0, &f->face))
		return NULL;
	return f->face;
}

static bool attr_true(VALUE font, const char *name)
{
	return RTEST(rb_ivar_get(font, rb_intern(name)));
}

static int font_px_size(VALUE font)
{
	VALUE s = rb_ivar_get(font, rb_intern("@size"));
	int n = NIL_P(s) ? 24 : num2int(s);
	return CLAMP(n, 6, 96);
}

static uint32_t next_cp(const unsigned char **p, const unsigned char *end)
{
	const unsigned char *s = *p;
	uint32_t c = *s++;
	int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (extra)
		c &= 0x3f >> extra;
	while (extra-- > 0 && s < end)
		c = (c << 6) | (*s++ & 0x3f);
	*p = s;
	return c;
}

/* the text's coverage mask: w x h, baseline at `base` */
typedef struct {
	uint8_t *a;
	int w, h;
} Mask;

static bool render_mask(VALUE font, VALUE str, Mask *m)
{
	FT_Face face = face_for(font);
	m->a = NULL;
	m->w = m->h = 0;
	if (!face)
		return false;
	int size = font_px_size(font);
	FT_Set_Pixel_Sizes(face, 0, size);
	bool bold = attr_true(font, "@bold"), italic = attr_true(font, "@italic");
	int asc = (int) (face->size->metrics.ascender >> 6);
	int desc = (int) (-face->size->metrics.descender >> 6);
	const unsigned char *s = (const unsigned char *) RSTRING_PTR(str), *end = s + RSTRING_LEN(str);
	/* first pass: width */
	int w = 0;
	for (const unsigned char *p = s; p < end;) {
		uint32_t c = next_cp(&p, end);
		if (FT_Load_Char(face, c, FT_LOAD_DEFAULT))
			continue;
		if (bold)
			FT_GlyphSlot_Embolden(face->glyph);
		w += (int) (face->glyph->advance.x >> 6);
	}
	m->w = w + 2;
	m->h = asc + desc;
	if (m->w <= 2 || m->h <= 0)
		return true;
	m->a = calloc((size_t) m->w * m->h, 1);
	if (!m->a)
		return false;
	int pen = 0;
	for (const unsigned char *p = s; p < end;) {
		uint32_t c = next_cp(&p, end);
		if (FT_Load_Char(face, c, FT_LOAD_DEFAULT))
			continue;
		if (bold)
			FT_GlyphSlot_Embolden(face->glyph);
		if (italic)
			FT_GlyphSlot_Oblique(face->glyph);
		if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
			continue;
		FT_Bitmap *g = &face->glyph->bitmap;
		int gx = pen + face->glyph->bitmap_left, gy = asc - face->glyph->bitmap_top;
		for (unsigned y = 0; y < g->rows; y++)
			for (unsigned x = 0; x < g->width; x++) {
				int tx = gx + (int) x, ty = gy + (int) y;
				if (tx < 0 || ty < 0 || tx >= m->w || ty >= m->h)
					continue;
				uint8_t v = g->buffer[y * g->pitch + x];
				uint8_t *d = &m->a[ty * m->w + tx];
				if (v > *d)
					*d = v;
			}
		pen += (int) (face->glyph->advance.x >> 6);
	}
	return true;
}

void font_text_size(VALUE font, VALUE str, int *w, int *h)
{
	FT_Face face = face_for(font);
	*w = 0;
	*h = font_px_size(font);
	if (!face)
		return;
	FT_Set_Pixel_Sizes(face, 0, *h);
	bool bold = attr_true(font, "@bold");
	const unsigned char *s = (const unsigned char *) RSTRING_PTR(str), *end = s + RSTRING_LEN(str);
	for (const unsigned char *p = s; p < end;) {
		uint32_t c = next_cp(&p, end);
		if (FT_Load_Char(face, c, FT_LOAD_DEFAULT))
			continue;
		if (bold)
			FT_GlyphSlot_Embolden(face->glyph);
		*w += (int) (face->glyph->advance.x >> 6);
	}
	int lh = (int) ((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
	if (lh > *h)
		*h = lh;
}

static Color font_color(VALUE font, const char *ivar, Color dflt)
{
	VALUE c = rb_ivar_get(font, rb_intern(ivar));
	return rb_obj_is_kind_of(c, cColor) ? *color_get(c) : dflt;
}

/* stamp the mask (horizontally scaled to sw) at (x, y), with a color */
static void stamp(Bitmap *b, const Mask *m, int sw, int x, int y, Color c, Rect clip)
{
	if (c.a <= 0)
		return;
	px_t col = PX((int) c.r, (int) c.g, (int) c.b, 0);
	for (int ty = 0; ty < m->h; ty++) {
		int yy = y + ty;
		if (yy < clip.y || yy >= clip.y + clip.h || yy < 0 || yy >= b->h)
			continue;
		px_t *row = b->px + (size_t) yy * b->w;
		for (int tx = 0; tx < sw; tx++) {
			int xx = x + tx;
			if (xx < clip.x || xx >= clip.x + clip.w || xx < 0 || xx >= b->w)
				continue;
			int mx = sw == m->w ? tx : tx * m->w / sw;
			int a = m->a[ty * m->w + mx] * (int) c.a / 255;
			if (!a)
				continue;
			/* over */
			px_t d = row[xx];
			int da = PX_A(d);
			int oa = a + da * (255 - a) / 255;
			if (!oa)
				continue;
			int r = (PX_R(col) * a + PX_R(d) * da * (255 - a) / 255) / oa;
			int g = (PX_G(col) * a + PX_G(d) * da * (255 - a) / 255) / oa;
			int bl = (PX_B(col) * a + PX_B(d) * da * (255 - a) / 255) / oa;
			row[xx] = PX(r, g, bl, oa);
		}
	}
}

void font_draw_text(Bitmap *b, VALUE font, int x, int y, int w, int h, VALUE str, int align)
{
	if (RSTRING_LEN(str) == 0 || w <= 0 || h <= 0)
		return;
	Mask m;
	if (!render_mask(font, str, &m) || !m.a)
		return;
	/* too wide: squeezed (RGSS shrinks the text to the rect, down to 60%) */
	int sw = m.w;
	if (sw > w + 2) {
		sw = w + 2;
		if (sw < m.w * 6 / 10)
			sw = m.w * 6 / 10;
	}
	int tx = align == 1 ? x + (w - sw) / 2 : align == 2 ? x + w - sw : x;
	int ty = y + (h - m.h) / 2;
	Rect clip = { x - 1, y - 1, w + 2, h + 2 };
	if (attr_true(font, "@shadow"))
		stamp(b, &m, sw, tx + 2, ty + 2, (Color) { 0, 0, 0, 255 }, clip);
	if (attr_true(font, "@outline")) {
		Color oc = font_color(font, "@out_color", (Color) { 0, 0, 0, 128 });
		/* 1 pixel around: the 8 neighbours, merged (not stacked) */
		Mask o = { calloc((size_t) m.w * m.h, 1), m.w, m.h };
		if (o.a) {
			for (int yy = 0; yy < m.h; yy++)
				for (int xx = 0; xx < m.w; xx++) {
					int best = 0;
					for (int j = -1; j <= 1; j++)
						for (int i = -1; i <= 1; i++) {
							int sx = xx + i, sy = yy + j;
							if (sx >= 0 && sy >= 0 && sx < m.w && sy < m.h && m.a[sy * m.w + sx] > best)
								best = m.a[sy * m.w + sx];
						}
					o.a[yy * m.w + xx] = best;
				}
			stamp(b, &o, sw, tx, ty, oc, clip);
			free(o.a);
		}
	}
	stamp(b, &m, sw, tx, ty, font_color(font, "@color", (Color) { 255, 255, 255, 255 }), clip);
	free(m.a);
}

void init_font(void)
{
	/* the class is defined in boot.rb; this adds Font.exist? */
	cFont = rb_define_class("Font", rb_cObject);
	rb_define_singleton_method(cFont, "exist?", font_exist, 1);
}
