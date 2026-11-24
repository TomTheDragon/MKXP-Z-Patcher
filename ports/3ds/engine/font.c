/* Text with FreeType. The Font class itself is plain Ruby (boot.rb); this
 * reads its attributes when drawing. Font files: the game's Fonts folder
 * and the engine's own fonts, found by family name. */
#include "engine.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

VALUE cFont;

typedef struct {
	char family[64];
	/* other names it's asked for by (Windows: the legacy family and full
	 * name, e.g. "Noto Sans S Chinese Medium" for the family "Noto Sans S
	 * Chinese" in the style "Medium") */
	char other[3][64];
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
		memset(f->other, 0, sizeof f->other);
		if (face->style_name && strcasecmp(face->style_name, "Regular"))
			snprintf(f->other[0], sizeof f->other[0], "%s %s", f->family, face->style_name);
		/* the name table: family (1) and full name (4), Windows entries
		 * (UTF-16BE, only plain ASCII names) */
		FT_UInt nn = FT_Get_Sfnt_Name_Count(face);
		for (FT_UInt k = 0; k < nn; k++) {
			FT_SfntName sn;
			if (FT_Get_Sfnt_Name(face, k, &sn) || sn.platform_id != TT_PLATFORM_MICROSOFT ||
			    (sn.name_id != TT_NAME_ID_FONT_FAMILY && sn.name_id != TT_NAME_ID_FULL_NAME))
				continue;
			char buf[64];
			size_t o = 0;
			bool ascii = true;
			for (FT_UInt b = 0; b + 1 < sn.string_len && o + 1 < sizeof buf; b += 2) {
				if (sn.string[b] || sn.string[b + 1] >= 0x80)
					ascii = false;
				buf[o++] = (char) sn.string[b + 1];
			}
			buf[o] = 0;
			if (!ascii || !o)
				continue;
			char *slot = f->other[sn.name_id == TT_NAME_ID_FONT_FAMILY ? 1 : 2];
			if (!*slot)
				snprintf(slot, 64, "%s", buf);
		}
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
	for (int i = 0; i < nfiles; i++)
		for (int k = 0; k < 3; k++)
			if (files[i].other[k][0] && !strcasecmp(files[i].other[k], name))
				return &files[i];
	return NULL;
}

/* fonts every Windows has (games check for them and quit without them,
 * Heartache 101: "Arial font not found"): drawn with the engine's font
 * (Liberation Sans is made to match Arial's widths), as MKXP-Z does */
static bool windows_standard(const char *name)
{
	static const char *const std[] = { "Arial", "Arial Black", "Tahoma", "Verdana", "Times New Roman", "Courier New",
	                                   "Georgia", "Trebuchet MS", "Segoe UI", "Microsoft Sans Serif", "MS Sans Serif",
	                                   "Lucida Console", "Comic Sans MS", "Impact", "MS Gothic", "MS PGothic",
	                                   "MS UI Gothic", "MS Mincho", "MS PMincho", "UmePlus Gothic", NULL };
	for (int i = 0; std[i]; i++)
		if (!strcasecmp(name, std[i]))
			return true;
	return false;
}

static VALUE font_exist(VALUE klass, VALUE name)
{
	const char *n = StringValueCStr(name);
	return find_family(n) || windows_standard(n) ? Qtrue : Qfalse;
}

/* the first available of the font's name(s), else the fallback */
static FT_Face face_for(VALUE font)
{
	VALUE name = rb_ivar_get(font, rb_intern("@name"));
	FontFile *f = NULL;
	if (RB_TYPE_P(name, T_ARRAY)) {
		for (long i = 0; i < RARRAY_LEN(name) && !f; i++)
			if (RB_TYPE_P(RARRAY_AREF(name, i), T_STRING)) {
				VALUE one = RARRAY_AREF(name, i);
				f = find_family(StringValueCStr(one));
			}
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

static float text_scale = 1; /* font_draw_text_scaled */
float font_box_scale = 1;     /* the bitmap's text_mult (bitmap.c: set around a call) */
float font_box_sx = 1;        /* and across, more (settings "Text width") */

/* a wide text box's text (mult: set for its contents): settings "Text
 * size" (both ways) and "Text width" (the letters stretched across) */
void font_box_begin(float mult)
{
	if (mult <= 0 || mult == 1) {
		font_box_scale = font_box_sx = 1;
		return;
	}
	font_box_scale = settings.text_size / 100.0f;
	font_box_sx = settings.text_width / 100.0f;
}
static int text_xs = 1;       /* letters this many times as wide (wide top screen) */

static int font_px_size(VALUE font)
{
	VALUE s = rb_ivar_get(font, rb_intern("@size"));
	int n = NIL_P(s) ? 24 : num2int(s);
	if (text_scale != 1 || font_box_scale != 1)
		n = (int) (n * text_scale * font_box_scale + 0.5f);
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

/* --- the letter cache: each letter (font, size, bold, italic) is rendered
 * once, with its outline mask (1 pixel around); FreeType is slow on the 3DS */
typedef struct {
	FT_Face face;
	uint32_t cp;
	unsigned char size, bold, italic, used, xs, sxp; /* (sxp: font_box_sx in percent) */
	short advance, left, top, w, h;
	uint8_t *cov;  /* w x h */
	uint8_t *out;  /* (w + 2) x (h + 2): the outline */
} Glyph;

#define GLYPHS 1024
static Glyph glyphs[GLYPHS];
static int nglyphs;

typedef struct {
	FT_Face face;
	int size;
	int asc, desc;
} Metrics;
static Metrics metrics[16];
static FT_Face sized_face;
static int sized_px, sized_xs, sized_sxp;

/* a font size as Windows (and mkxp-z, from Wine) takes it: the height of
 * the letters' cell (win ascent + descent), not of the em square; the em
 * then is smaller (e.g. Changed's Source Han Sans at 25: 18 pixels; ours
 * was 25, and long lines didn't fit into the text box) */
static int windows_ppem(FT_Face face, int height)
{
	if (!FT_IS_SCALABLE(face) || height <= 0)
		return height;
	TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
	TT_HoriHeader *hh = FT_Get_Sfnt_Table(face, FT_SFNT_HHEA);
	long units = 0;
	if (os2 && os2->version != 0xFFFF)
		units = os2->usWinAscent + labs((long) (short) os2->usWinDescent);
	if (units <= 0 && hh)
		units = hh->Ascender - hh->Descender;
	if (units <= 0)
		return height;
	int ppem = (int) FT_MulDiv(face->units_per_EM, height, units);
	if (ppem > 1 && FT_MulDiv(units, ppem, face->units_per_EM) > height)
		ppem--;
	return ppem > 0 ? ppem : 1;
}

static void set_size(FT_Face face, int size)
{
	int sxp = (int) (font_box_sx * 100 + 0.5f);
	if (sized_face != face || sized_px != size || sized_xs != text_xs || sized_sxp != sxp) {
		int ppem = windows_ppem(face, size);
		FT_Set_Pixel_Sizes(face, (FT_UInt) (ppem * text_xs * font_box_sx + 0.5f), ppem);
		sized_face = face;
		sized_px = size;
		sized_xs = text_xs;
		sized_sxp = sxp;
	}
}

static const Metrics *metrics_of(FT_Face face, int size)
{
	for (int i = 0; i < 16; i++)
		if (metrics[i].face == face && metrics[i].size == size)
			return &metrics[i];
	static int next;
	Metrics *m = &metrics[next++ % 16];
	set_size(face, size);
	m->face = face;
	m->size = size;
	m->asc = (int) (face->size->metrics.ascender >> 6);
	m->desc = (int) (-face->size->metrics.descender >> 6);
	return m;
}

static void glyphs_clear(void)
{
	for (int i = 0; i < GLYPHS; i++) {
		free(glyphs[i].cov);
		free(glyphs[i].out);
		glyphs[i].cov = glyphs[i].out = NULL;
		glyphs[i].face = NULL;
	}
	nglyphs = 0;
}

/* a letter the font doesn't have (e.g. Chinese text, a game asking for
 * "SimHei" that isn't there): from another font that has it, the game's
 * own fonts first (as Windows takes missing letters from other fonts).
 * Remembered per block of 256 letters. */
static FT_Face fallback_face(FT_Face face, uint32_t cp)
{
	if (cp < 0x80 || FT_Get_Char_Index(face, cp))
		return face;
	static struct { FT_Face face, use; uint32_t block; } memo[32];
	static int next;
	for (int i = 0; i < 32; i++)
		if (memo[i].face == face && memo[i].block == cp >> 8 && memo[i].use && FT_Get_Char_Index(memo[i].use, cp))
			return memo[i].use;
	for (int i = nfiles - 1; i >= 0; i--) { /* (the game's fonts come last) */
		FontFile *f = &files[i];
		if (!f->face && FT_New_Face(ft, f->path, 0, &f->face)) {
			f->face = NULL;
			continue;
		}
		if (f->face != face && FT_Get_Char_Index(f->face, cp)) {
			memo[next].face = face;
			memo[next].use = f->face;
			memo[next].block = cp >> 8;
			next = (next + 1) % 32;
			return f->face;
		}
	}
	return face;
}

static const Glyph *glyph_of(FT_Face face, int size, bool bold, bool italic, uint32_t cp)
{
	face = fallback_face(face, cp);
	uint32_t h = (cp * 2654435761u) ^ ((uint32_t) (uintptr_t) face >> 4) ^ (uint32_t) (size << 3 | bold << 1 | italic) ^ (uint32_t) text_xs << 12;
	for (int n = 0, i = h % GLYPHS; n < 16; n++, i = (i + 1) % GLYPHS) {
		Glyph *g = &glyphs[i];
		if (!g->face)
			break;
		if (g->face == face && g->cp == cp && g->size == size && g->bold == bold && g->italic == italic && g->xs == text_xs &&
		    g->sxp == (unsigned char) (font_box_sx * 100 + 0.5f))
			return g;
	}
	if (nglyphs > GLYPHS * 3 / 4)
		glyphs_clear(); /* full: start over */
	Glyph *g = NULL;
	for (int n = 0, i = h % GLYPHS; n < GLYPHS; n++, i = (i + 1) % GLYPHS)
		if (!glyphs[i].face) {
			g = &glyphs[i];
			break;
		}
	if (!g)
		return NULL;
	set_size(face, size);
	memset(g, 0, sizeof *g);
	g->face = face;
	g->cp = cp;
	g->size = size;
	g->bold = bold;
	g->italic = italic;
	g->xs = (unsigned char) text_xs;
	g->sxp = (unsigned char) (font_box_sx * 100 + 0.5f);
	nglyphs++;
	/* small sizes (e.g. sharp window text, ~14 px): light auto-hinting
	 * puts horizontal strokes on whole pixels */
	if (FT_Load_Char(face, cp, size <= 18 ? FT_LOAD_TARGET_LIGHT | FT_LOAD_FORCE_AUTOHINT : FT_LOAD_DEFAULT))
		return g; /* (no such letter: empty) */
	if (bold)
		FT_GlyphSlot_Embolden(face->glyph);
	if (italic)
		FT_GlyphSlot_Oblique(face->glyph);
	g->advance = (short) (face->glyph->advance.x >> 6);
	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
		return g;
	FT_Bitmap *bm = &face->glyph->bitmap;
	g->left = (short) face->glyph->bitmap_left;
	g->top = (short) face->glyph->bitmap_top;
	g->w = (short) bm->width;
	g->h = (short) bm->rows;
	if (g->w > 0 && g->h > 0) {
		g->cov = malloc((size_t) g->w * g->h);
		g->out = calloc((size_t) (g->w + 2) * (g->h + 2), 1);
		if (!g->cov || !g->out) {
			free(g->cov);
			free(g->out);
			g->cov = g->out = NULL;
			g->w = g->h = 0;
			return g;
		}
		for (int y = 0; y < g->h; y++)
			memcpy(g->cov + y * g->w, bm->buffer + y * bm->pitch, g->w);
		/* the outline: each pixel's 3x3 neighbourhood, at its strongest */
		int ow = g->w + 2;
		for (int y = 0; y < g->h; y++)
			for (int x = 0; x < g->w; x++) {
				uint8_t v = g->cov[y * g->w + x];
				if (!v)
					continue;
				for (int j = 0; j < 3; j++)
					for (int i = 0; i < 3; i++) {
						uint8_t *d = &g->out[(y + j) * ow + x + i];
						if (v > *d)
							*d = v;
					}
			}
	}
	return g;
}

/* the text's coverage masks (letters and outline), w x h; baseline at asc */
typedef struct {
	uint8_t *a, *o;
	int w, h;
} Mask;

static bool render_mask(VALUE font, VALUE str, Mask *m, bool outline)
{
	FT_Face face = face_for(font);
	m->a = m->o = NULL;
	m->w = m->h = 0;
	if (!face)
		return false;
	int size = font_px_size(font);
	bool bold = attr_true(font, "@bold"), italic = attr_true(font, "@italic");
	const Metrics *mt = metrics_of(face, size);
	const unsigned char *s = (const unsigned char *) RSTRING_PTR(str), *end = s + RSTRING_LEN(str);
	int w = 0;
	for (const unsigned char *p = s; p < end;) {
		const Glyph *g = glyph_of(face, size, bold, italic, next_cp(&p, end));
		if (g)
			w += g->advance;
	}
	m->w = w + 2;
	m->h = mt->asc + mt->desc;
	if (m->w <= 2 || m->h <= 0)
		return true;
	m->a = calloc((size_t) m->w * m->h, 1);
	m->o = outline ? calloc((size_t) m->w * m->h, 1) : NULL;
	if (!m->a || (outline && !m->o)) {
		free(m->a);
		free(m->o);
		m->a = m->o = NULL;
		return false;
	}
	int pen = 1;
	for (const unsigned char *p = s; p < end;) {
		const Glyph *g = glyph_of(face, size, bold, italic, next_cp(&p, end));
		if (!g)
			continue;
		int gx = pen + g->left, gy = mt->asc - g->top;
		for (int y = 0; y < g->h; y++) {
			int ty = gy + y;
			if (ty < 0 || ty >= m->h)
				continue;
			for (int x = 0; x < g->w; x++) {
				int tx = gx + x;
				if (tx < 0 || tx >= m->w)
					continue;
				uint8_t v = g->cov[y * g->w + x];
				uint8_t *d = &m->a[ty * m->w + tx];
				if (v > *d)
					*d = v;
			}
		}
		if (outline && g->out) {
			int ow = g->w + 2;
			for (int y = 0; y < g->h + 2; y++) {
				int ty = gy + y - 1;
				if (ty < 0 || ty >= m->h)
					continue;
				for (int x = 0; x < ow; x++) {
					int tx = gx + x - 1;
					if (tx < 0 || tx >= m->w)
						continue;
					uint8_t v = g->out[y * ow + x];
					uint8_t *d = &m->o[ty * m->w + tx];
					if (v > *d)
						*d = v;
				}
			}
		}
		pen += g->advance;
	}
	return true;
}

void font_text_size(VALUE font, VALUE str, int *w, int *h)
{
	FT_Face face = face_for(font);
	int size = font_px_size(font);
	*w = 0;
	*h = size;
	if (!face)
		return;
	bool bold = attr_true(font, "@bold"), italic = attr_true(font, "@italic");
	const unsigned char *s = (const unsigned char *) RSTRING_PTR(str), *end = s + RSTRING_LEN(str);
	for (const unsigned char *p = s; p < end;) {
		const Glyph *g = glyph_of(face, size, bold, italic, next_cp(&p, end));
		if (g)
			*w += g->advance;
	}
	const Metrics *mt = metrics_of(face, size);
	if (mt->asc + mt->desc > *h)
		*h = mt->asc + mt->desc;
}

static Color font_color(VALUE font, const char *ivar, Color dflt)
{
	VALUE c = rb_ivar_get(font, rb_intern(ivar));
	return rb_obj_is_kind_of(c, cColor) ? *color_get(c) : dflt;
}

/* stamp the mask (horizontally scaled to sw) at (x, y), with a color */
/* 65536 / n */
static uint32_t recip[256];

static void stamp(Bitmap *b, const Mask *m, int sw, int x, int y, Color c, Rect clip)
{
	if (!recip[1])
		for (int n = 1; n < 256; n++)
			recip[n] = 65536 / n;
	if (c.a <= 0)
		return;
	px_t col = PX((int) c.r, (int) c.g, (int) c.b, 0);
	int ca = (int) c.a;
	/* the visible columns (clip and bitmap) */
	int tx0 = 0, tx1 = sw;
	if (x + tx0 < clip.x) tx0 = clip.x - x;
	if (x + tx0 < 0) tx0 = -x;
	if (x + tx1 > clip.x + clip.w) tx1 = clip.x + clip.w - x;
	if (x + tx1 > b->w) tx1 = b->w - x;
	if (tx1 <= tx0)
		return;
	/* squeezed text: the mask column of each column, once */
	int *cols = NULL;
	if (sw != m->w) {
		cols = malloc(sizeof *cols * sw);
		if (!cols)
			return;
		for (int tx = 0; tx < sw; tx++)
			cols[tx] = tx * m->w / sw;
	}
	for (int ty = 0; ty < m->h; ty++) {
		int yy = y + ty;
		if (yy < clip.y || yy >= clip.y + clip.h || yy < 0 || yy >= b->h)
			continue;
		px_t *row = b->px + (size_t) yy * b->w + x;
		const uint8_t *mrow = m->a + ty * m->w;
		for (int tx = tx0; tx < tx1; tx++) {
			int mv = mrow[cols ? cols[tx] : tx];
			if (!mv)
				continue;
			int a = ca == 255 ? mv : mv * ca / 255;
			px_t d = row[tx];
			int da = PX_A(d);
			/* fast cases: nothing below (cleared contents), or fully covered */
			if (da == 0 || a == 255) {
				row[tx] = col | (px_t) a << 24;
				continue;
			}
			/* "over" without divisions: x / 255 ~ (x * 257 + 257) >> 16,
			 * 1 / oa from a table */
			int keep = ((da * (255 - a)) * 257 + 257) >> 16;  /* da * (1 - a) */
			int oa = a + keep;
			uint32_t inv = recip[oa];
			int r = ((PX_R(col) * a + PX_R(d) * keep) * inv) >> 16;
			int g = ((PX_G(col) * a + PX_G(d) * keep) * inv) >> 16;
			int bl = ((PX_B(col) * a + PX_B(d) * keep) * inv) >> 16;
			row[tx] = PX(r > 255 ? 255 : r, g > 255 ? 255 : g, bl > 255 ? 255 : bl, oa);
		}
	}
	free(cols);
}

void font_draw_text(Bitmap *b, VALUE font, int x, int y, int w, int h, VALUE str, int align)
{
	if (RSTRING_LEN(str) == 0 || w <= 0 || h <= 0)
		return;
	bool outline = attr_true(font, "@outline");
	Mask m;
	if (!render_mask(font, str, &m, outline) || !m.a)
		return;
	/* too wide: squeezed (RGSS shrinks the text to the rect, down to 60%) */
	int sw = m.w;
	/* (text box text made bigger / wider: never squeezed, nor cut at the
	 * rect, which the game sized for its own letters (VX: 40 per letter)) */
	bool boxed = font_box_scale != 1 || font_box_sx != 1;
	if (sw > w + 2 && !boxed) {
		sw = w + 2;
		if (sw < m.w * 6 / 10)
			sw = m.w * 6 / 10;
	}
	int tx = align == 1 ? x + (w - sw) / 2 : align == 2 ? x + w - sw : x;
	int ty = y + (h - m.h) / 2;
	/* (across: the rect; up and down: the letters' own height, when it's
	 * more than the rect's: our fonts' ascent + descent is a bit more than
	 * Windows', and the rect cut the bottom of the letters off) */
	int extra = m.h > h ? (m.h - h) / 2 + 2 : 1;
	Rect clip = { x - 1, y - extra, (boxed && sw + 2 > w + 2 ? sw : w) + 2, h + 2 * extra };
	Mask only = m;
	int sh = text_scale < 0.75f ? 1 : 2; /* (the shadow's offset, at the scale) */
	if (attr_true(font, "@shadow"))
		stamp(b, &only, sw, tx + sh * text_xs, ty + sh, (Color) { 0, 0, 0, 255 }, clip);
	if (outline && m.o) {
		Mask o = { m.o, NULL, m.w, m.h };
		stamp(b, &o, sw, tx, ty, font_color(font, "@out_color", (Color) { 0, 0, 0, 128 }), clip);
	}
	stamp(b, &only, sw, tx, ty, font_color(font, "@color", (Color) { 255, 255, 255, 255 }), clip);
	free(m.a);
	free(m.o);
}

/* the same text with the font size times scale (sharp text at the top
 * screen's resolution: the rect is already scaled) */
void font_draw_text_scaled(Bitmap *b, VALUE font, int x, int y, int w, int h, VALUE str, int align, float scale, int xs)
{
	text_scale = scale;
	text_xs = xs;
	font_draw_text(b, font, x, y, w, h, str, align);
	text_scale = 1;
	text_xs = 1;
}

void init_font(void)
{
	/* the class is defined in boot.rb; this adds Font.exist? */
	cFont = rb_define_class("Font", rb_cObject);
	rb_define_singleton_method(cFont, "exist?", font_exist, 1);
}
