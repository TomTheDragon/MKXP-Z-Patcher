/* The game list: one app (MKXP-Z 3DS.3dsx) for all games. Each folder in
 * sdmc:/mkxpz with a Game.ini is a game; its name and icon come from
 * game.smdh (written by tools/make-package.py), else Game.ini's Title and a
 * plain icon. Drawn straight into the top screen's framebuffer (FreeType,
 * the built-in font) before Ruby and the GPU start; the chosen game runs
 * in the same app.
 *
 * Back to the list after a game: the app is started again by the Homebrew
 * Launcher's loader (Luma3DS's hb:ldr, as the Homebrew Launcher itself
 * starts apps: the next app to load is set, then this one quits). */
#include "engine.h"
#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define GAMES_DIR "sdmc:/mkxpz"

typedef struct {
	char dir[300];
	char name[192];   /* UTF-8 */
	u16 *icon;        /* 48x48 RGB565, or NULL */
} Game;

static Game *games;
static int ngames;
int menu_games; /* how many the list had (1: started at once, no list) */

/* --- reading a game's name and icon */

/* UTF-16 (little endian, n units at most) into UTF-8 */
static void utf16_to_8(const unsigned char *s, int n, char *out, size_t size)
{
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		unsigned c = s[2 * i] | s[2 * i + 1] << 8;
		if (!c)
			break;
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
			unsigned d = s[2 * i + 2] | s[2 * i + 3] << 8;
			c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
			i++;
		}
		char b[4];
		int k;
		if (c < 0x80) b[0] = (char) c, k = 1;
		else if (c < 0x800) b[0] = (char) (0xC0 | c >> 6), b[1] = (char) (0x80 | (c & 63)), k = 2;
		else if (c < 0x10000) b[0] = (char) (0xE0 | c >> 12), b[1] = (char) (0x80 | ((c >> 6) & 63)), b[2] = (char) (0x80 | (c & 63)), k = 3;
		else b[0] = (char) (0xF0 | c >> 18), b[1] = (char) (0x80 | ((c >> 12) & 63)), b[2] = (char) (0x80 | ((c >> 6) & 63)), b[3] = (char) (0x80 | (c & 63)), k = 4;
		if (o + k >= size)
			break;
		memcpy(out + o, b, k);
		o += k;
	}
	out[o] = 0;
}

/* an SMDH's 48x48 icon (8x8 tiles, Morton order inside) into rows */
static u16 *untile_icon(const u16 *tiled)
{
	u16 *icon = malloc(48 * 48 * 2);
	if (!icon)
		return NULL;
	for (int y = 0; y < 48; y++)
		for (int x = 0; x < 48; x++) {
			int m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
			icon[y * 48 + x] = tiled[((y >> 3) * 6 + (x >> 3)) * 64 + m];
		}
	return icon;
}

static bool read_smdh(Game *g)
{
	char p[340];
	snprintf(p, sizeof p, "%s/game.smdh", g->dir);
	FILE *f = fopen(p, "rb");
	if (!f)
		return false;
	static unsigned char smdh[0x36C0];
	bool ok = fread(smdh, 1, sizeof smdh, f) == sizeof smdh && !memcmp(smdh, "SMDH", 4);
	fclose(f);
	if (!ok)
		return false;
	/* the English title (language 1), else the Japanese one */
	utf16_to_8(smdh + 8 + 0x200, 0x40, g->name, sizeof g->name);
	if (!g->name[0])
		utf16_to_8(smdh + 8, 0x40, g->name, sizeof g->name);
	g->icon = untile_icon((const u16 *) (smdh + 0x24C0));
	return true;
}

/* Game.ini's Title (UTF-8, else Windows-1252 read as Latin-1) */
static void ini_title(Game *g)
{
	char p[340], line[256];
	snprintf(p, sizeof p, "%s/Game.ini", g->dir);
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		if (strncasecmp(line, "Title=", 6))
			continue;
		char *v = line + 6;
		v[strcspn(v, "\r\n")] = 0;
		bool utf8 = true;
		for (const unsigned char *s = (const unsigned char *) v; *s;) {
			int k = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2 : (*s & 0xF8) == 0xF0 ? 3 : -1;
			if (k < 0) { utf8 = false; break; }
			s++;
			for (; k > 0; k--, s++)
				if ((*s & 0xC0) != 0x80) { utf8 = false; break; }
			if (!utf8)
				break;
		}
		size_t o = 0;
		for (const unsigned char *s = (const unsigned char *) v; *s && o + 3 < sizeof g->name; s++) {
			if (utf8 || *s < 0x80) {
				g->name[o++] = (char) *s;
			} else {
				g->name[o++] = (char) (0xC0 | *s >> 6);
				g->name[o++] = (char) (0x80 | (*s & 63));
			}
		}
		g->name[o] = 0;
		break;
	}
	fclose(f);
}

static bool is_game(const char *dir)
{
	char p[340];
	snprintf(p, sizeof p, "%s/Game.ini", dir);
	struct stat st;
	return stat(p, &st) == 0;
}

static void add_game(const char *dir, const char *folder)
{
	Game *n = realloc(games, sizeof *games * (ngames + 1));
	if (!n)
		return;
	games = n;
	Game *g = &games[ngames++];
	memset(g, 0, sizeof *g);
	snprintf(g->dir, sizeof g->dir, "%s", dir);
	if (!read_smdh(g) || !g->name[0])
		ini_title(g);
	if (!g->name[0])
		snprintf(g->name, sizeof g->name, "%s", folder);
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const Game *) a)->name, ((const Game *) b)->name);
}

static void scan_games(void)
{
	if (is_game(GAMES_DIR))
		add_game(GAMES_DIR, "mkxpz");
	DIR *d = opendir(GAMES_DIR);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[300];
		snprintf(p, sizeof p, GAMES_DIR "/%s", e->d_name);
		if (is_game(p))
			add_game(p, e->d_name);
	}
	if (d)
		closedir(d);
	qsort(games, ngames, sizeof *games, by_name);
}

/* --- drawing: into a picture of the top screen's framebuffer layout (240
 * pixels per column, bottom up, BGR; or the bottom screen's, RGB565),
 * copied to the screen when complete. Texts are rendered once (labels). */
static u8 *fb;      /* the top screen picture being drawn */
static u16 *fb16;   /* or the bottom screen (RGB565, 320 wide) */
static int fb_w = 400;

static inline void put(int x, int y, int r, int g, int b, int a)
{
	if (x < 0 || x >= fb_w || y < 0 || y >= 240 || a <= 0)
		return;
	if (fb16) {
		u16 *p = fb16 + (size_t) x * 240 + (239 - y);
		if (a < 255) {
			int pr = (*p >> 11) << 3, pg = ((*p >> 5) & 63) << 2, pb = (*p & 31) << 3;
			r = (pr * (255 - a) + r * a) / 255, g = (pg * (255 - a) + g * a) / 255, b = (pb * (255 - a) + b * a) / 255;
		}
		*p = (u16) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		return;
	}
	u8 *p = fb + ((size_t) x * 240 + (239 - y)) * 3;
	if (a >= 255) {
		p[0] = (u8) b, p[1] = (u8) g, p[2] = (u8) r;
	} else {
		p[0] = (u8) ((p[0] * (255 - a) + b * a) / 255);
		p[1] = (u8) ((p[1] * (255 - a) + g * a) / 255);
		p[2] = (u8) ((p[2] * (255 - a) + r * a) / 255);
	}
}

static void rect(int x, int y, int w, int h, u32 rgb)
{
	if (x < 0) w += x, x = 0;
	if (y < 0) h += y, y = 0;
	if (x + w > fb_w) w = fb_w - x;
	if (y + h > 240) h = 240 - y;
	if (w <= 0 || h <= 0)
		return;
	u8 r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
	for (int i = x; i < x + w; i++) {
		if (fb16) {
			u16 v = (u16) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
			u16 *p = fb16 + (size_t) i * 240 + (240 - y - h);
			for (int k = 0; k < h; k++)
				p[k] = v;
		} else {
			u8 *p = fb + ((size_t) i * 240 + (240 - y - h)) * 3;
			for (int k = 0; k < h; k++, p += 3)
				p[0] = b, p[1] = g, p[2] = r;
		}
	}
}

static FT_Library ft;
static FT_Face face;

static void font_open(void)
{
	if (face)
		return;
	if (!ft && FT_Init_FreeType(&ft))
		ft = NULL;
	if (ft && FT_New_Face(ft, "romfs:/fonts/liberation.ttf", 0, &face))
		face = NULL;
}

static void font_close(void)
{
	if (face)
		FT_Done_Face(face);
	if (ft)
		FT_Done_FreeType(ft);
	face = NULL;
	ft = NULL;
}

/* the next character of UTF-8 text */
static unsigned next_char(const char **s)
{
	const unsigned char *p = (const unsigned char *) *s;
	unsigned c = *p++;
	int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
	if (k)
		c &= 0x3F >> k;
	for (; k > 0 && (*p & 0xC0) == 0x80; k--)
		c = c << 6 | (*p++ & 63);
	*s = (const char *) p;
	return c;
}

/* a text rendered once: its coverage, w x h, the baseline at y = asc */
typedef struct { u8 *a; int w, h, asc; } Label;

static int advance_of(unsigned c)
{
	return FT_Load_Char(face, c, FT_LOAD_DEFAULT) ? 0 : (int) (face->glyph->advance.x >> 6);
}

/* s into l from pen on, up to limit; the new pen */
static int render_into(Label *l, int pen, const char *s, int limit)
{
	for (const char *t = s; *t;) {
		unsigned c = next_char(&t);
		if (FT_Load_Char(face, c, FT_LOAD_RENDER))
			continue;
		FT_GlyphSlot g = face->glyph;
		int adv = (int) (g->advance.x >> 6);
		if (pen + adv > limit)
			break;
		for (unsigned r = 0; r < g->bitmap.rows; r++)
			for (unsigned col = 0; col < g->bitmap.width; col++) {
				int x = pen + g->bitmap_left + (int) col, y = l->asc - g->bitmap_top + (int) r;
				if (x >= 0 && x < l->w && y >= 0 && y < l->h) {
					int v = l->a[y * l->w + x] + g->bitmap.buffer[r * g->bitmap.pitch + col];
					l->a[y * l->w + x] = (u8) (v > 255 ? 255 : v);
				}
			}
		pen += adv;
	}
	return pen;
}

/* a text, "..." at the end when wider than maxw */
static Label label(const char *s, int px, int maxw)
{
	Label l = { NULL, 0, 0, 0 };
	if (!face)
		return l;
	FT_Set_Pixel_Sizes(face, 0, px);
	int full = 0, dots = 3 * advance_of('.');
	for (const char *t = s; *t;)
		full += advance_of(next_char(&t));
	bool cut = full > maxw;
	l.asc = px + 2;
	l.h = px * 3 / 2 + 4;
	l.w = cut ? maxw : full;
	if (l.w <= 0 || !(l.a = calloc((size_t) l.w, (size_t) l.h)))
		return (Label) { NULL, 0, 0, 0 };
	int pen = render_into(&l, 0, s, cut ? maxw - dots : maxw);
	if (cut)
		render_into(&l, pen, "...", maxw);
	return l;
}

/* a label at x, its baseline at y */
static void draw_label(const Label *l, int x, int y, u32 rgb)
{
	for (int j = 0; j < l->h; j++)
		for (int i = 0; i < l->w; i++) {
			int a = l->a[j * l->w + i];
			if (a)
				put(x + i, y - l->asc + j, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, a);
		}
}

static void text(int x, int y, int px, const char *s, u32 rgb, int w)
{
	Label l = label(s, px, w);
	draw_label(&l, x, y, rgb);
	free(l.a);
}

static Label *names; /* per game, rendered once */

static void icon(int x, int y, int i)
{
	const Game *g = &games[i];
	if (!g->icon) {
		/* (no icon: a plain one with the name's first letter) */
		rect(x, y, 48, 48, 0x3c4658);
		char first[8] = { 0 };
		const char *s = g->name;
		unsigned c = next_char(&s);
		if (c < 0x80)
			first[0] = (char) c;
		text(x + 14, y + 34, 26, first, 0xdfe6f0, 40);
		return;
	}
	for (int j = 0; j < 48; j++)
		for (int k = 0; k < 48; k++) {
			u16 p = g->icon[j * 48 + k];
			int r = (p >> 11) & 31, gg = (p >> 5) & 63, b = p & 31;
			put(x + k, y + j, r << 3 | r >> 2, gg << 2 | gg >> 4, b << 3 | b >> 2, 255);
		}
}

#define HEAD 34
#define ROW 51
#define ROWS 4

static u8 *picture; /* the list's picture (top screen layout) */
static Label head_label, count_label;

static void draw_list(int sel, int first)
{
	fb = picture;
	fb16 = NULL;
	fb_w = 400;
	rect(0, 0, 400, 240, 0x15181e);
	rect(0, 0, 400, HEAD - 4, 0x232833);
	draw_label(&head_label, 10, 22, 0xffffff);
	draw_label(&count_label, 400 - 10 - count_label.w, 22, 0x9aa4b4);
	if (!ngames) {
		text(16, 90, 15, "No games found.", 0xffffff, 370);
		text(16, 120, 13, "Put each game into its own folder in", 0xb8c0cc, 370);
		text(16, 140, 13, "sdmc:/mkxpz/ (with its Game.ini).", 0xb8c0cc, 370);
	}
	for (int k = 0; k < ROWS && first + k < ngames; k++) {
		int i = first + k, y = HEAD + k * ROW;
		if (i == sel)
			rect(4, y, 392 - 8, ROW - 2, 0x34507e);
		icon(8, y + 1, i);
		draw_label(&names[i], 66, y + 31, i == sel ? 0xffffff : 0xd0d6e0);
	}
	/* where the list is (more than fit) */
	if (ngames > ROWS) {
		int h = 240 - HEAD - 4, bh = h * ROWS / ngames;
		if (bh < 12)
			bh = 12;
		int by = HEAD + (h - bh) * first / (ngames - ROWS);
		rect(394, HEAD, 3, h, 0x232833);
		rect(394, by, 3, bh, 0x7d8aa0);
	}
}

/* the picture onto the top screen (the buffer being shown next) */
static void show_picture(void)
{
	u8 *dst = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	memcpy(dst, picture, 400 * 240 * 3);
	gfxFlushBuffers();
	gfxSwapBuffers();
}

static void bottom_help(int sel)
{
	printf("\x1b[2J\x1b[1;1H");
	printf("MKXP-Z 3DS\n\n");
	if (ngames) {
		printf("Up/Down: choose   A: start\n");
		printf("L/R: a page up/down\n");
		printf("START: back to the Homebrew Launcher\n\n");
		printf("%s\n", games[sel].dir);
	} else {
		printf("START: back to the Homebrew Launcher\n");
	}
}

/* the list; the chosen game's folder into dir (false: quit) */
bool menu_pick(char *dir, size_t n)
{
	scan_games();
	menu_games = ngames;
	if (ngames == 1) {
		/* (only one game: started at once) */
		snprintf(dir, n, "%s", games[0].dir);
		free(games[0].icon);
		free(games);
		games = NULL;
		ngames = 0;
		return true;
	}
	font_open();
	picture = malloc(400 * 240 * 3);
	names = calloc(ngames ? ngames : 1, sizeof *names);
	for (int i = 0; i < ngames; i++)
		names[i] = label(games[i].name, 17, 400 - 66 - 18);
	head_label = label("MKXP-Z 3DS", 17, 200);
	char count[32];
	snprintf(count, sizeof count, ngames == 1 ? "1 game" : "%d games", ngames);
	count_label = label(count, 14, 100);
	gfxSet3D(false);
	hidSetRepeatParameters(12, 3);
	int sel = 0, first = 0, drawn = -1, pending = 0;
	bool chosen = false;
	console_force = true;
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), rep = hidKeysDownRepeat();
		if (ngames) {
			if (rep & (KEY_DUP | KEY_CPAD_UP))
				sel = (sel + ngames - 1) % ngames;
			if (rep & (KEY_DDOWN | KEY_CPAD_DOWN))
				sel = (sel + 1) % ngames;
			if (rep & (KEY_DLEFT | KEY_L))
				sel = sel - ROWS < 0 ? 0 : sel - ROWS;
			if (rep & (KEY_DRIGHT | KEY_R))
				sel = sel + ROWS >= ngames ? ngames - 1 : sel + ROWS;
			if (down & KEY_A) {
				chosen = true;
				break;
			}
		}
		if (down & KEY_START)
			break;
		if (sel < first)
			first = sel;
		if (sel >= first + ROWS)
			first = sel - ROWS + 1;
		if (drawn != sel) {
			draw_list(sel, first);
			bottom_help(sel);
			drawn = sel;
			pending = 2; /* (both screen buffers) */
		}
		if (pending && picture) {
			show_picture();
			pending--;
		}
		gspWaitForVBlank();
	}
	if (chosen)
		snprintf(dir, n, "%s", games[sel].dir);
	/* the top screen black again for the game's loading bar */
	for (int i = 0; i < 2; i++) {
		u8 *t = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
		memset(t, 0, 400 * 240 * 3);
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	printf("\x1b[2J\x1b[1;1H");
	console_force = false;
	for (int i = 0; i < ngames; i++) {
		free(names[i].a);
		free(games[i].icon);
	}
	free(head_label.a);
	free(count_label.a);
	free(names);
	free(picture);
	free(games);
	names = NULL;
	picture = NULL;
	games = NULL;
	ngames = 0;
	font_close();
	return chosen;
}

/* after a game: the question on the bottom screen, big (an error's text
 * stays above it); true: back to the list */
bool menu_ask(bool error)
{
	font_open();
	u16 *bottom = (u16 *) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	fb = NULL;
	fb16 = bottom;
	fb_w = 320;
	int top = error ? 128 : 0;
	rect(0, top, 320, 240 - top, 0x15181e);
	if (!error)
		text(16, 52, 22, "The game has ended.", 0xffffff, 300);
	int y = error ? top + 40 : 120;
	text(16, y, 18, menu_games == 1 ? "A: start the game again" : "A: back to the game list", 0xffffff, 300);
	text(16, y + 40, 18, "B: exit to the Homebrew Launcher", 0xd0d6e0, 300);
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
	/* (the same on the other buffer) */
	u16 *other = (u16 *) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	if (other != bottom)
		memcpy(other, bottom, 320 * 240 * 2);
	gfxFlushBuffers();
	fb16 = NULL;
	fb_w = 400;
	bool back = false;
	while (aptMainLoop()) {
		hidScanInput();
		u32 k = hidKeysDown();
		if (k & KEY_A) {
			back = true;
			break;
		}
		if (k & (KEY_B | KEY_START))
			break;
		gspWaitForVBlank();
	}
	font_close();
	return back;
}

/* the next app the loader starts: this one again (the list); false: no
 * hb:ldr (e.g. an emulator, or not started by the Homebrew Launcher) */
bool menu_relaunch(const char *self)
{
	Handle h;
	if (!self || !self[0] || R_FAILED(svcConnectToPort(&h, "hb:ldr")))
		return false;
	const char *path = !strncmp(self, "sdmc:/", 6) ? self + 5 : self;
	u32 *cmd = getThreadCommandBuffer();
	cmd[0] = IPC_MakeHeader(2, 0, 2); /* SetTarget */
	cmd[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
	cmd[2] = (u32) path;
	Result rc = svcSendSyncRequest(h);
	if (R_SUCCEEDED(rc))
		rc = (Result) cmd[1];
	if (R_SUCCEEDED(rc)) {
		/* argv: its count, then the strings (argv[0]: the app's path) */
		static u32 args[0x400 / 4];
		memset(args, 0, sizeof args);
		args[0] = 1;
		snprintf((char *) &args[1], sizeof args - 4, "%s", self);
		cmd = getThreadCommandBuffer();
		cmd[0] = IPC_MakeHeader(3, 0, 2); /* SetArgv */
		cmd[1] = IPC_Desc_StaticBuffer(sizeof args, 1);
		cmd[2] = (u32) args;
		rc = svcSendSyncRequest(h);
		if (R_SUCCEEDED(rc))
			rc = (Result) cmd[1];
	}
	svcCloseHandle(h);
	return R_SUCCEEDED(rc);
}
