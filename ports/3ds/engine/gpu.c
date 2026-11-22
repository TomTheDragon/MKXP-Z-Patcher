/* Drawing with the 3DS GPU (citro3d). Bitmaps become textures (uploaded
 * when they change); everything is drawn as quads in game coordinates,
 * projected onto the top screen (scaled to fit). Opacity, color and tone go
 * through the GPU's six combiner stages; blending: normal, add, subtract. */
#include "engine.h"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vshader_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget *top;
static DVLB_s *vsh;
static shaderProgram_s prog;
static int u_proj;
static C3D_Mtx proj_scene, proj_top, proj_ui;
/* the top screen's width in pixels: 400, or 800 in wide mode (settings
 * "Top screen"; the panel has 800 columns, 3D shows them to one eye each):
 * twice the detail across, half-wide pixels. gpu_top_xs: 1 or 2. Things
 * of the engine's own (battery, loading bar) are drawn on 400 (proj_ui). */
static int top_w = 400;
int gpu_top_xs = 1;
static void top_target(void);
static bool in_frame;
static float scale_x, scale_y, off_x, off_y; /* game -> top screen */
static Rect cur_clip = { -1, -1, -1, -1 };
static int cur_blend = -1;
static int game_w = 544, game_h = 416;

/* the scene is drawn at the game's resolution into a texture, then scaled
 * onto the top screen with filtering (drawing it scaled directly loses thin
 * lines); a second texture holds the frozen picture (Graphics.freeze) */
#define SCENE_W 1024
#define SCENE_H 512
static C3D_Tex scene_tex[2];
static C3D_RenderTarget *scene_rt[2];

/* A bitmap's texture is made of up to 8x8 pieces with power-of-two sides
 * (the GPU's rule): a 544x544 picture as 512+32 by 512+32 needs 1.1 MB,
 * as one 1024x1024 texture 4 MB (which often didn't fit).
 *
 * Sparse textures: big images (4 MB and more, e.g. Changed's 3072x2048
 * character sheets: 24 MB, more than all of the linear memory) get their
 * pieces only when a piece is drawn: it is marked (want), and after the
 * frame the marked pieces are filled, from the pixels when the image has
 * them, else from its file (a sprite shows one 256x256 cell of such a
 * sheet: one 1 MB piece instead of 24 MB). Pieces not drawn lately make
 * room when memory runs out. An image drawn into refills its pieces. */
#define MAXP 8
#define SPARSE_MIN (4u * 1024 * 1024)
typedef struct GpuTex GpuTex;
struct GpuTex {
	C3D_Tex tex[MAXP * MAXP]; /* row by row */
	int nx, ny;               /* pieces across, down */
	int xs[MAXP + 1], ys[MAXP + 1]; /* their edges in the bitmap */
	size_t bytes;
	int w, h; /* bitmap size it was made for */
	bool transparent; /* nothing visible: not drawn at all */
	bool repeat;      /* wrap mode set to repeat (Plane) */
	bool sparse;      /* pieces made when drawn (see above) */
	u64 have, want;   /* sparse: pieces there, pieces to read after the frame */
	unsigned piece_used[MAXP * MAXP]; /* sparse: frame each piece was last drawn */
	Bitmap *owner;
	unsigned last_used; /* frame */
	GpuTex *prev, *next; /* the live textures, most recently used first */
};

/* textures of disposed bitmaps: freed once the GPU is done with them */
static void tex_delete(GpuTex *g)
{
	for (int k = 0; k < g->nx * g->ny; k++)
		if (g->tex[k].data)
			C3D_TexDelete(&g->tex[k]);
}

static GpuTex **trash;
static int ntrash, trash_cap;

static void to_trash(GpuTex *g)
{
	if (ntrash == trash_cap) {
		int nc = trash_cap ? trash_cap * 2 : 64;
		GpuTex **nt = realloc(trash, sizeof *nt * nc);
		if (!nt) {
			tex_delete(g); /* (no memory: free it now) */
			free(g);
			return;
		}
		trash = nt;
		trash_cap = nc;
	}
	trash[ntrash++] = g;
}

/* the live textures: when memory runs low, the least recently used ones go
 * (their bitmaps upload them again when drawn) */
static GpuTex *live_first, *live_last;
static unsigned frame_no = 1;
#define LINEAR_RESERVE (1024 * 1024) /* for sound, vertex buffers */

static void live_remove(GpuTex *g)
{
	if (g->prev) g->prev->next = g->next; else if (live_first == g) live_first = g->next;
	if (g->next) g->next->prev = g->prev; else if (live_last == g) live_last = g->prev;
	g->prev = g->next = NULL;
}

static void live_touch(GpuTex *g)
{
	g->last_used = frame_no;
	if (live_first == g)
		return;
	live_remove(g);
	g->next = live_first;
	if (live_first)
		live_first->prev = g;
	live_first = g;
	if (!live_last)
		live_last = g;
}

/* free the least recently used texture not used in this frame */
static bool evict_one(void)
{
	GpuTex *g = live_last;
	if (!g || g->last_used == frame_no)
		return false;
	live_remove(g);
	if (g->owner)
		g->owner->gpu = NULL;
	tex_delete(g); /* (the GPU finished the frames that used it) */
	free(g);
	return true;
}

unsigned gpu_evictions;
static unsigned frame_used_kb(void);

static unsigned frame_used_kb(void)
{
	unsigned kb = 0;
	for (GpuTex *g = live_first; g && g->last_used == frame_no; g = g->next)
		kb += g->bytes / 1024;
	return kb;
}

/* KB of textures used in the last frame, and how many */
unsigned gpu_used_kb(unsigned *count)
{
	unsigned kb = 0, n = 0;
	for (GpuTex *g = live_first; g && g->last_used + 1 >= frame_no; g = g->next) {
		kb += g->bytes / 1024;
		n++;
	}
	*count = n;
	return kb;
}

bool gpu_on;
/* statistics (shown with the frame times) */
unsigned gpu_uploads, gpu_upload_px, gpu_draws;
u64 gpu_upload_ticks;

void gpu_init(void)
{
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	top_target();
	for (int i = 0; i < 2; i++) {
		if (!C3D_TexInitVRAM(&scene_tex[i], SCENE_W, SCENE_H, GPU_RGBA8))
			C3D_TexInit(&scene_tex[i], SCENE_W, SCENE_H, GPU_RGBA8);
		C3D_TexSetFilter(&scene_tex[i], GPU_LINEAR, GPU_LINEAR);
		scene_rt[i] = C3D_RenderTargetCreateFromTex(&scene_tex[i], GPU_TEXFACE_2D, 0, -1);
	}
	vsh = DVLB_ParseFile((u32 *) vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &vsh->DVLE[0]);
	C3D_BindProgram(&prog);
	u_proj = shaderInstanceGetUniformLocation(prog.vertexShader, "projection");
	C3D_AttrInfo *ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3); /* position */
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2); /* texture coordinate */
	AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 4); /* color */
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	/* the scene texture: game pixels, y down; the top screen: its pixels */
	Mtx_Ortho(&proj_scene, 0.0f, SCENE_W, 0.0f, SCENE_H, 0.0f, 1.0f, true);
	Mtx_OrthoTilt(&proj_ui, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
	gpu_on = true;
}

/* wide mode works everywhere but on the original 2DS */
bool gpu_wide_possible(void)
{
	static int possible = -1;
	if (possible < 0) {
		u8 model = 0;
		possible = 1;
		if (R_SUCCEEDED(cfguInit())) {
			if (R_SUCCEEDED(CFGU_GetSystemModel(&model)) && model == CFG_MODEL_2DS)
				possible = 0;
			cfguExit();
		}
	}
	return possible;
}

/* an emulator of the Citra family (Citra, Lime3DS, Azahar): it answers
 * svcGetSystemInfo's type 0x20000 ("Citra information", 0: which emulator;
 * Citra 1, Azahar 2); a real 3DS (and Luma3DS) has no such type and
 * returns an error */
bool system_is_emulator(void)
{
	static int emu = -1;
	if (emu < 0) {
		s64 out = 0;
		Result rc = svcGetSystemInfo(&out, 0x20000, 0);
		emu = R_SUCCEEDED(rc) && out != 0;
		log_only("system: %s (svcGetSystemInfo 0x20000: %08lx, %lld)\n", emu ? "emulator" : "3DS",
		         (unsigned long) rc, (long long) out);
	}
	return emu;
}

/* "auto": wide where it shows (not the original 2DS, not an emulator,
 * which displays only the left half of the 800 columns) */
bool gpu_wide_auto(void)
{
	return gpu_wide_possible() && !system_is_emulator();
}

/* the top screen's render target, for the setting (between frames) */
static void top_target(void)
{
	bool wide = settings.wide_screen == 2 ? gpu_wide_auto() : settings.wide_screen == 1 && gpu_wide_possible();
	if (top) {
		C3D_FrameSync();
		C3D_RenderTargetDelete(top);
	}
	gfxSetWide(wide);
	top_w = wide ? 800 : 400;
	gpu_top_xs = wide ? 2 : 1;
	top = C3D_RenderTargetCreate(240, top_w, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	Mtx_OrthoTilt(&proj_top, 0.0f, (float) top_w, 240.0f, 0.0f, 0.0f, 1.0f, true);
}

void gpu_set_wide(void)
{
	if (!gpu_on || in_frame)
		return;
	top_target();
	gpu_set_screen(game_w, game_h);
}

int gpu_margin;

/* the game's screen size: how it fits onto the top screen */
void gpu_set_screen(int w, int h)
{
	if (!gpu_on)
		return;
	game_w = w;
	game_h = h;
	/* fit with wide text boxes: the picture as wide as the screen (5:3);
	 * the game draws in the middle, its message boxes over all of it */
	gpu_margin = 0;
	if (settings.screen == 0 && settings.wide_text && h * 400 > w * 240)
		gpu_margin = (h * 400 / 240 - w) / 2;
	if (w + 2 * gpu_margin > SCENE_W)
		gpu_margin = (SCENE_W - w) / 2;
	Mtx_Ortho(&proj_scene, (float) -gpu_margin, SCENE_W - gpu_margin, 0.0f, SCENE_H, 0.0f, 1.0f, true);
	w += 2 * gpu_margin;
	if (settings.screen == 1) {
		/* stretched to fill the screen */
		scale_x = (float) top_w / w;
		scale_y = 240.0f / h;
	} else {
		/* the whole picture, aspect ratio kept (wide: pixels half as wide) */
		scale_y = w * 240 > h * 400 ? 400.0f / w : 240.0f / h;
		scale_x = scale_y * gpu_top_xs;
	}
	off_x = (top_w - w * scale_x) / 2;
	off_y = (240 - h * scale_y) / 2;
}

static bool presenting;

static void set_clip(Rect r)
{
	if (presenting || !memcmp(&r, &cur_clip, sizeof r))
		return;
	cur_clip = r;
	int x0 = CLAMP(r.x + gpu_margin, 0, SCENE_W), y0 = CLAMP(r.y, 0, SCENE_H);
	int x1 = CLAMP(r.x + r.w + gpu_margin, 0, SCENE_W), y1 = CLAMP(r.y + r.h, 0, SCENE_H);
	C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1);
}

static void set_blend(int mode)
{
	if (mode == cur_blend)
		return;
	cur_blend = mode;
	switch (mode) {
	case 1:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
		break;
	case 2:
		C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
		break;
	default:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
		break;
	}
}

static inline u32 abgr(int r, int g, int b, int a)
{
	return (u32) CLAMP(r, 0, 255) | (u32) CLAMP(g, 0, 255) << 8 | (u32) CLAMP(b, 0, 255) << 16 | (u32) CLAMP(a, 0, 255) << 24;
}

/* the combiner stages: texture (or vertex color), gray, tone, color */
/* the combiner setup last made (skipped when the same again) */
static struct { bool valid, textured; Color color; Tone tone; } tev_last;

static void set_tev(bool textured, const Color *color, const Tone *tone)
{
	Color cz = { 0, 0, 0, 0 };
	Tone tz = { 0, 0, 0, 0 };
	Color ck = color && color->a > 0 ? *color : cz;
	Tone tk = tone ? *tone : tz;
	if (tev_last.valid && tev_last.textured == textured && !memcmp(&tev_last.color, &ck, sizeof ck)
	    && !memcmp(&tev_last.tone, &tk, sizeof tk))
		return;
	tev_last.valid = true;
	tev_last.textured = textured;
	tev_last.color = ck;
	tev_last.tone = tk;
	int i = 0;
	C3D_TexEnv *e;
	bool gray = textured && tone && tone->gray > 0;
	if (gray) {
		/* (tex + 1) / 2, then dot3 with weights -> luminance */
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_MULTIPLY_ADD);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e, abgr(128, 128, 128, 255));
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_DOT3_RGB);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e, abgr(166, 202, 142, 255));
		/* luminance * gray + texture * (1 - gray) */
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE0, GPU_CONSTANT);
		C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e, abgr(0, 0, 0, (int) tone->gray));
	} else {
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		if (textured) {
			C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
			C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
		} else {
			C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
		}
	}
	if (tone && (tone->r > 0 || tone->g > 0 || tone->b > 0)) {
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_ADD);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e, abgr(tone->r > 0 ? (int) tone->r : 0, tone->g > 0 ? (int) tone->g : 0,
		                        tone->b > 0 ? (int) tone->b : 0, 255));
	}
	if (tone && (tone->r < 0 || tone->g < 0 || tone->b < 0)) {
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_SUBTRACT);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e, abgr(tone->r < 0 ? (int) -tone->r : 0, tone->g < 0 ? (int) -tone->g : 0,
		                        tone->b < 0 ? (int) -tone->b : 0, 255));
	}
	if (color && color->a > 0) {
		/* color * alpha + previous * (1 - alpha) */
		e = C3D_GetTexEnv(i++);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_CONSTANT);
		C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e, abgr((int) color->r, (int) color->g, (int) color->b, (int) color->a));
	}
	for (; i < 6; i++)
		C3D_TexEnvInit(C3D_GetTexEnv(i));
}

static int pow2(int n)
{
	int p = 8;
	while (p < n)
		p <<= 1;
	return p;
}

/* an axis in pieces: a power of two (at most 512, or 1024 for axes over
 * 2048) as long as one texture for the rest would be more than a quarter
 * padding; 0: too long. (Pieces of at most 1 MB: a 1536x1024 sheet as one
 * 4 MB and one 2 MB block didn't fit any more into the linear memory in
 * pieces after a few resets, and the character was missing.) */
static int split_axis(int n, int *edges)
{
	int k = 0, at = 0, most = n > 512 * MAXP ? 1024 : 512;
	edges[0] = 0;
	while (at < n && k < MAXP) {
		int r = n - at, p = pow2(r), len = r;
		if (k < MAXP - 1 && p > 8 && p - r > r / 4)
			len = p / 2;
		if (len > most)
			len = most;
		at += len;
		edges[++k] = at;
	}
	return at == n ? k : 0;
}

static bool tex_init(GpuTex *g)
{
	for (int j = 0; j < g->ny; j++)
		for (int i = 0; i < g->nx; i++)
			if (!C3D_TexInit(&g->tex[j * g->nx + i], pow2(g->xs[i + 1] - g->xs[i]), pow2(g->ys[j + 1] - g->ys[j]),
			                 GPU_RGBA8)) {
				for (int k = 0; k < j * g->nx + i; k++)
					C3D_TexDelete(&g->tex[k]);
				return false;
			}
	return true;
}


static unsigned long long last_frame_end;
bool gpu_in_frame(void) { return in_frame; }
unsigned long long gpu_last_frame_end(void) { return last_frame_end; }

/* big images read straight into their textures (bitmap_stream_file) wait
 * until the frame is done: then the loading bar can show (they appear a
 * frame later) */
static Bitmap *pending[32];
static int npending;
unsigned gpu_null_pending, gpu_null_alloc, gpu_null_px; /* (for the log) */

/* rows y0 .. y0+n-1 of the bitmap (px: those rows, stride pixels apart)
 * into the texture pieces: RGBA8 in 8x8 tiles (Morton order inside); the
 * GPU reads memory row r at v = 1 - r / height, so bitmap row y goes to
 * row y */
typedef struct { GpuTex *g; u32 alpha_or; u64 mask; /* 0: all pieces */ } UploadCtx;
static void upload_rows(UploadCtx *u, int y0, int n, const px_t *px, int stride)
{
	GpuTex *g = u->g;
	u32 alpha_or = u->alpha_or;
	for (int k = 0; k < g->nx * g->ny; k++) {
		if ((u->mask && !((u->mask >> k) & 1)) || !g->tex[k].data)
			continue;
		int i = k % g->nx, j = k / g->nx, x0 = g->xs[i], pw = g->xs[i + 1] - x0;
		int ya = y0 > g->ys[j] ? y0 : g->ys[j], yb = y0 + n < g->ys[j + 1] ? y0 + n : g->ys[j + 1];
		if (ya >= yb)
			continue;
		u32 *dst = g->tex[k].data;
		int tw = g->tex[k].width;
		for (int y = ya; y < yb; y++) {
			int ty = y - g->ys[j];
			const px_t *src = px + (size_t) (y - y0) * stride + x0;
			u32 rowbase = (u32) ((ty >> 3) * (tw >> 3)) << 6;
			u32 my = ((ty & 1) << 1) | ((ty & 2) << 2) | ((ty & 4) << 3);
			for (int x = 0; x < pw; x++) {
				u32 mx = (x & 1) | ((x & 2) << 1) | ((x & 4) << 2);
				dst[rowbase + ((u32) (x >> 3) << 6) + (mx | my)] = __builtin_bswap32(src[x]);
				alpha_or |= src[x];
			}
		}
	}
	u->alpha_or = alpha_or;
}

static bool upload_rows_cb(void *ctx, int y0, int n, const px_t *px)
{
	UploadCtx *u = ctx;
	upload_rows(u, y0, n, px, u->g->w);
	return true;
}

static void queue_pending(Bitmap *b)
{
	for (int i = 0; i < npending; i++)
		if (pending[i] == b)
			return;
	if (npending < (int) (sizeof pending / sizeof *pending))
		pending[npending++] = b;
}

/* a sparse texture's piece k: made (cleared) */
static bool piece_init(GpuTex *g, int k)
{
	int i = k % g->nx, j = k / g->nx;
	C3D_Tex *t = &g->tex[k];
	if (!C3D_TexInit(t, pow2(g->xs[i + 1] - g->xs[i]), pow2(g->ys[j + 1] - g->ys[j]), GPU_RGBA8))
		return false;
	memset(t->data, 0, t->size);
	C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	g->bytes += t->size;
	return true;
}

/* a sparse texture's pieces not drawn in this frame (and not in keep): let
 * go of; true if there was one */
static bool pieces_drop(GpuTex *g, u64 keep)
{
	bool any = false;
	for (int k = 0; k < g->nx * g->ny; k++)
		if (((g->have >> k) & 1) && !((keep >> k) & 1) && g->piece_used[k] != frame_no) {
			g->bytes -= g->tex[k].size;
			C3D_TexDelete(&g->tex[k]);
			memset(&g->tex[k], 0, sizeof g->tex[k]);
			g->have &= ~(1ull << k);
			any = true;
		}
	return any;
}

/* after the frame: the pieces drawn but not there yet, read from the file */
static void sparse_fill(GpuTex *g, Bitmap *b)
{
	u64 todo = g->want & ~g->have;
	g->want = 0;
	u64 made = 0;
	for (int k = 0; k < g->nx * g->ny; k++) {
		if (!((todo >> k) & 1))
			continue;
		bool ok = piece_init(g, k);
		while (!ok && evict_one()) {
			gpu_evictions++;
			ok = piece_init(g, k);
		}
		if (!ok && pieces_drop(g, todo))
			ok = piece_init(g, k);
		if (ok)
			made |= 1ull << k;
		else
			gpu_null_alloc++;
	}
	if (!made)
		return;
	u64 t_up = svcGetSystemTick();
	gpu_uploads++;
	UploadCtx up = { g, 0, made };
	if (b->px)
		upload_rows(&up, 0, b->h, b->px, b->w);
	else if (!bitmap_stream_file(b, upload_rows_cb, &up))
		log_only("%s: couldn't read pieces of it\n", b->file ? b->file : "(image)");
	for (int k = 0; k < g->nx * g->ny; k++)
		if ((made >> k) & 1) {
			C3D_TexFlush(&g->tex[k]);
			gpu_upload_px += (unsigned) (g->tex[k].width * g->tex[k].height);
		}
	g->have |= made; /* (also when reading failed: not tried again every frame) */
	gpu_upload_ticks += svcGetSystemTick() - t_up;
}

/* the bitmap's texture, uploaded if the bitmap changed */
static GpuTex *texture_of(Bitmap *b)
{
	GpuTex *g = b->gpu;
	if (g && g->sparse) {
		if (g->w == b->w && g->h == b->h && (b->px || b->dropped)) {
			live_touch(g);
			if (b->gpu_version != b->version + 1) {
				/* drawn into (it has its pixels then): the pieces there
				 * filled again now, the others when they're drawn */
				if (b->px && g->have) {
					UploadCtx up = { g, 0, g->have };
					upload_rows(&up, 0, b->h, b->px, b->w);
					for (int k = 0; k < g->nx * g->ny; k++)
						if ((g->have >> k) & 1)
							C3D_TexFlush(&g->tex[k]);
					gpu_uploads++;
				}
				b->gpu_version = b->version + 1;
			}
			if (!in_frame && (g->want & ~g->have))
				sparse_fill(g, b);
			return g;
		}
		live_remove(g);
		to_trash(g);
		b->gpu = NULL;
		g = NULL;
	}
	if (g && g->w == b->w && g->h == b->h && b->gpu_version == b->version + 1) {
		live_touch(g);
		return g;
	}
	/* an image without its pixels, during drawing: read from its file after
	 * the frame (gpu_frame_end; then the loading bar can show), the texture
	 * made only then too (one made during the frame and filled after it
	 * came out empty after a while: characters and tiles went missing) */
	if (!g && (b->px || b->dropped)) {
		GpuTex t = { 0 };
		t.nx = split_axis(b->w, t.xs);
		t.ny = split_axis(b->h, t.ys);
		size_t need = 0;
		for (int j = 0; j < t.ny; j++)
			for (int i = 0; i < t.nx; i++)
				need += (size_t) pow2(t.xs[i + 1] - t.xs[i]) * pow2(t.ys[j + 1] - t.ys[j]) * 4;
		if (t.nx && t.ny && need >= SPARSE_MIN && (g = malloc(sizeof *g))) {
			/* a sparse texture: no pieces yet (draw_pieces asks for them) */
			*g = t;
			g->sparse = true;
			g->owner = b;
			g->w = b->w;
			g->h = b->h;
			b->gpu = g;
			b->gpu_version = b->version + 1;
			live_touch(g);
			return g;
		}
	}
	if (in_frame && !b->px && b->dropped) {
		queue_pending(b);
		gpu_null_pending++;
		return NULL;
	}
	if (!g || g->w != b->w || g->h != b->h) {
		if (g) {
			live_remove(g);
			to_trash(g);
			b->gpu = NULL;
		}
		GpuTex t = { 0 };
		t.nx = split_axis(b->w, t.xs);
		t.ny = split_axis(b->h, t.ys);
		if (!t.nx || !t.ny)
			return NULL; /* (over 4096 pixels) */
		size_t need = 0;
		for (int j = 0; j < t.ny; j++)
			for (int i = 0; i < t.nx; i++)
				need += (size_t) pow2(t.xs[i + 1] - t.xs[i]) * pow2(t.ys[j + 1] - t.ys[j]) * 4;
		while (linearSpaceFree() < need + LINEAR_RESERVE && evict_one())
			gpu_evictions++;
		g = malloc(sizeof *g);
		if (g)
			*g = t;
		bool ok = g && tex_init(g);
		while (g && !ok && evict_one()) {
			gpu_evictions++;
			ok = tex_init(g);
		}
		if (!ok) {
			static unsigned fails;
			if (fails++ < 30)
				log_only("texture %dx%d (%u KB) failed: free %lu KB, used this frame %u KB\n", b->w, b->h,
				         (unsigned) (need / 1024), (unsigned long) (linearSpaceFree() / 1024), frame_used_kb());
			free(g);
			gpu_null_alloc++;
			return NULL;
		}
		g->owner = b;
		g->bytes = need;
		for (int k = 0; k < g->nx * g->ny; k++) {
			memset(g->tex[k].data, 0, g->tex[k].size);
			C3D_TexSetFilter(&g->tex[k], GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&g->tex[k], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
		g->w = b->w;
		g->h = b->h;
		b->gpu = g;
	}
	u64 t_up = svcGetSystemTick();
	gpu_uploads++;
	gpu_upload_px += (unsigned) (b->w * b->h);
	UploadCtx up = { g, 0 };
	/* an image without its pixels: straight from its file, in rows; else
	 * from the pixels (read again if needed: only this image's are in use) */
	bool streamed = !b->px && b->dropped && bitmap_stream_file(b, upload_rows_cb, &up);
	if (!streamed) {
		if (!bitmap_px_alone(b)) {
			gpu_null_px++;
			return NULL;
		}
		up.alpha_or = 0;
		upload_rows(&up, 0, b->h, b->px, b->w);
	}
	for (int k = 0; k < g->nx * g->ny; k++)
		C3D_TexFlush(&g->tex[k]);
	u32 alpha_or = up.alpha_or;
	g->transparent = (alpha_or >> 24) == 0;
	b->gpu_version = b->version + 1;
	live_touch(g);
	gpu_upload_ticks += svcGetSystemTick() - t_up;
	bitmap_uploaded(b);
	return g;
}

/* (tests) the texture's state as text */
void gpu_tex_info(Bitmap *b, char *out, size_t n)
{
	GpuTex *g = b->gpu;
	if (!g) {
		snprintf(out, n, "no tex, px=%d dropped=%d ver=%u", b->px != NULL, b->dropped, b->version);
		return;
	}
	u32 sum = 0;
	for (int k = 0; k < g->nx * g->ny; k++) {
		const u32 *d = g->tex[k].data;
		for (size_t i = 0; i < g->tex[k].size / 4; i += 61)
			sum |= d[i];
	}
	snprintf(out, n, "tex %dx%d pieces %dx%d current=%d transparent=%d sample=%08lx owner_ok=%d px=%d dropped=%d",
	         g->w, g->h, g->nx, g->ny, b->gpu_version == b->version + 1, g->transparent, (unsigned long) sum,
	         g->owner == b, b->px != NULL, b->dropped);
}

void gpu_forget(Bitmap *b)
{
	if (b->gpu) {
		GpuTex *g = b->gpu;
		live_remove(g);
		g->owner = NULL;
		if (gpu_on)
			to_trash(g);
		b->gpu = NULL;
	}
}

static void vertex(float x, float y, float u, float v, float a)
{
	C3D_ImmSendAttrib(x, y, 0.5f, 0.0f);
	C3D_ImmSendAttrib(u, v, 0.0f, 0.0f);
	C3D_ImmSendAttrib(1.0f, 1.0f, 1.0f, a);
}

/* a textured quad: corners (x0,y0)-(x1,y1) rotated by angle around (cx, cy) */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float a,
                 float angle, float cx, float cy)
{
	float px[4] = { x0, x0, x1, x1 }, py[4] = { y0, y1, y0, y1 };
	float pu[4] = { u0, u0, u1, u1 }, pv[4] = { v0, v1, v0, v1 };
	if (angle != 0) {
		/* RGSS: counterclockwise in degrees */
		float s = sinf(-angle * (float) M_PI / 180), c = cosf(-angle * (float) M_PI / 180);
		for (int i = 0; i < 4; i++) {
			float dx = px[i] - cx, dy = py[i] - cy;
			px[i] = cx + dx * c - dy * s;
			py[i] = cy + dx * s + dy * c;
		}
	}
	gpu_draws++;
	C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
	for (int i = 0; i < 4; i++)
		vertex(px[i], py[i], pu[i], pv[i], a);
	C3D_ImmDrawEnd();
}

static const Tone *merge_tone(const Tone *a, const Tone *b, Tone *out)
{
	bool av = a && (a->r || a->g || a->b || a->gray), bv = b && (b->r || b->g || b->b || b->gray);
	if (!av)
		return bv ? b : NULL;
	if (!bv)
		return a;
	out->r = CLAMP(a->r + b->r, -255, 255);
	out->g = CLAMP(a->g + b->g, -255, 255);
	out->b = CLAMP(a->b + b->b, -255, 255);
	out->gray = a->gray > b->gray ? a->gray : b->gray;
	return out;
}

/* the part sr of the bitmap, its top left corner (unmirrored) at (ox, oy),
 * with each piece's share as its own quad */
static void draw_pieces(GpuTex *g, Rect sr, float ox, float oy, const DrawOpts *o, float a)
{
	float zx = (float) o->zx, zy = (float) o->zy;
	for (int j = 0; j < g->ny; j++) {
		int r0 = sr.y > g->ys[j] ? sr.y : g->ys[j], r1 = sr.y + sr.h < g->ys[j + 1] ? sr.y + sr.h : g->ys[j + 1];
		if (r0 >= r1)
			continue;
		for (int i = 0; i < g->nx; i++) {
			int c0 = sr.x > g->xs[i] ? sr.x : g->xs[i], c1 = sr.x + sr.w < g->xs[i + 1] ? sr.x + sr.w : g->xs[i + 1];
			if (c0 >= c1)
				continue;
			int k = j * g->nx + i;
			if (g->sparse) {
				g->piece_used[k] = frame_no;
				if (!((g->have >> k) & 1)) {
					/* not there yet: read after the frame */
					g->want |= 1ull << k;
					queue_pending(g->owner);
					continue;
				}
			}
			C3D_Tex *t = &g->tex[k];
			C3D_TexBind(0, t);
			float tw = t->width, th = t->height;
			float y0 = oy + (r0 - sr.y) * zy, y1 = oy + (r1 - sr.y) * zy;
			float v0 = 1 - (r0 - g->ys[j]) / th, v1 = 1 - (r1 - g->ys[j]) / th;
			float x0, x1, u0, u1;
			if (!o->mirror) {
				x0 = ox + (c0 - sr.x) * zx;
				x1 = ox + (c1 - sr.x) * zx;
				u0 = (c0 - g->xs[i]) / tw;
				u1 = (c1 - g->xs[i]) / tw;
			} else {
				/* column c lands at ox + (sr.x + sr.w - c) * zx */
				x0 = ox + (sr.x + sr.w - c1) * zx;
				x1 = ox + (sr.x + sr.w - c0) * zx;
				u0 = (c1 - g->xs[i]) / tw;
				u1 = (c0 - g->xs[i]) / tw;
			}
			quad(x0, y0, x1, y1, u0, v0, u1, v1, a, (float) o->angle, o->cx, o->cy);
		}
	}
}

void gpu_draw(Canvas *c, Bitmap *src, Rect sr, const DrawOpts *o)
{
	if (sr.x < 0) { sr.w += sr.x; sr.x = 0; }
	if (sr.y < 0) { sr.h += sr.y; sr.y = 0; }
	if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
	if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
	if (sr.w <= 0 || sr.h <= 0 || o->opacity <= 0 || o->zx == 0 || o->zy == 0)
		return;
	GpuTex *g = texture_of(src);
	if (!g || g->transparent)
		return;
	if (g->repeat) {
		C3D_TexSetWrap(&g->tex[0], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		g->repeat = false;
	}
	set_clip(c->clip);
	set_blend(o->blend);
	Tone merged;
	set_tev(true, o->color, merge_tone(o->tone, c->tone, &merged));
	float a = o->opacity / 255.0f;
	if (o->bush_depth > 0 && o->bush_depth < sr.h) {
		/* the lower bush_depth rows: less opaque */
		int split = sr.h - o->bush_depth;
		draw_pieces(g, (Rect) { sr.x, sr.y, sr.w, split }, o->x, o->y, o, a);
		draw_pieces(g, (Rect) { sr.x, sr.y + split, sr.w, sr.h - split }, o->x, o->y + split * (float) o->zy, o,
		            a * o->bush_opacity / 255.0f);
		return;
	}
	draw_pieces(g, sr, o->x, o->y, o, a);
}

void gpu_fill(Canvas *c, Rect r, const Color *col, int alpha)
{
	if (alpha <= 0)
		return;
	set_clip(c->clip);
	set_blend(0);
	set_tev(false, NULL, NULL);
	C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
	float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
	float cr = col->r / 255.0f, cg = col->g / 255.0f, cb = col->b / 255.0f, ca = alpha / 255.0f;
	float px[4] = { x0, x0, x1, x1 }, py[4] = { y0, y1, y0, y1 };
	for (int i = 0; i < 4; i++) {
		C3D_ImmSendAttrib(px[i], py[i], 0.5f, 0.0f);
		C3D_ImmSendAttrib(0, 0, 0, 0);
		C3D_ImmSendAttrib(cr, cg, cb, ca);
	}
	C3D_ImmDrawEnd();
}

void gpu_frame_begin(void)
{
	gpu_frame_begin_sync(true);
}

/* sync: wait for the screen's next refresh first (when the game is on time;
 * late, it draws at once and the picture shows at the next refresh) */
void gpu_frame_begin_sync(bool sync)
{
	C3D_FrameBegin(sync ? C3D_FRAME_SYNCDRAW : 0);
	in_frame = true;
	frame_no++;
	/* the GPU is done with the last frame: free textures of disposed bitmaps */
	for (int i = 0; i < ntrash; i++) {
		tex_delete(trash[i]);
		free(trash[i]);
	}
	ntrash = 0;
}

/* draw the scene into texture 0 (the picture) or 1 (the frozen one) */
/* --- sharp window text: collected while the scene is drawn, drawn at the
 * top screen's resolution on the finished picture (before a transition's
 * frozen picture and the brightness) */
typedef struct { Bitmap *cont; int gx, gy; Rect clip; int opacity; } TextJob;
static TextJob text_jobs[64];
static int ntext_jobs, scene_which;

void gpu_text_layer(Bitmap *cont, int gx, int gy, Rect clip, int opacity)
{
	if (scene_which == 1) {
		/* the frozen picture (Graphics.freeze): the text at game size into
		 * it (that layer is let go of after the freeze) */
		Bitmap *l = bitmap_text_layer(cont, 1, 1);
		if (l) {
			Canvas c = { .w = game_w, .h = game_h, .clip = clip, .gpu = true };
			DrawOpts o = { .x = gx, .y = gy, .zx = 1, .zy = 1, .opacity = opacity };
			gpu_draw(&c, l, (Rect) { 0, 0, l->w, l->h }, &o);
		}
		return;
	}
	if (ntext_jobs < (int) (sizeof text_jobs / sizeof *text_jobs) && opacity > 0)
		text_jobs[ntext_jobs++] = (TextJob) { cont, gx, gy, clip, opacity };
}

static void draw_text_jobs(void)
{
	Canvas c = { .w = top_w, .h = 240, .clip = { 0, 0, top_w, 240 }, .gpu = true };
	for (int i = 0; i < ntext_jobs; i++) {
		TextJob *j = &text_jobs[i];
		Bitmap *l = bitmap_text_layer(j->cont, scale_x, scale_y);
		if (!l)
			continue;
		/* the contents' origin on the screen, whole pixels (sharp) */
		int ox = (int) floorf(off_x + (j->gx + gpu_margin) * scale_x + 0.5f);
		int oy = (int) floorf(off_y + j->gy * scale_y + 0.5f);
		/* what shows: the clip, in the layer's pixels */
		int x0 = (int) floorf((j->clip.x - j->gx) * scale_x), y0 = (int) floorf((j->clip.y - j->gy) * scale_y);
		int x1 = (int) ceilf((j->clip.x + j->clip.w - j->gx) * scale_x);
		int y1 = (int) ceilf((j->clip.y + j->clip.h - j->gy) * scale_y);
		if (x1 <= x0 || y1 <= y0)
			continue;
		DrawOpts o = { .x = ox + x0, .y = oy + y0, .zx = 1, .zy = 1, .opacity = j->opacity };
		gpu_draw(&c, l, (Rect) { x0, y0, x1 - x0, y1 - y0 }, &o);
	}
}

void gpu_scene_begin(int which)
{
	scene_which = which;
	if (which == 0)
		ntext_jobs = 0;
	C3D_RenderTargetClear(scene_rt[which], C3D_CLEAR_ALL, 0x000000ff, 0);
	C3D_FrameDrawOn(scene_rt[which]);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj_scene);
	presenting = false;
	tev_last.valid = false;
	cur_clip = (Rect) { -1, -1, -1, -1 };
	cur_blend = -1;
}

/* a scene texture onto the top screen, scaled with filtering */
static void present_tex(int which, float alpha)
{

	set_blend(0);
	set_tev(true, NULL, NULL);
	if (settings.colors) {
		/* vivid: 1.2 * c - 0.1 (contrast around the middle) */
		C3D_TexEnv *e = C3D_GetTexEnv(0);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_2);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_MODULATE);
		C3D_TexEnvColor(e, abgr(153, 153, 153, 255));
		e = C3D_GetTexEnv(1);
		C3D_TexEnvInit(e);
		C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(e, C3D_RGB, GPU_SUBTRACT);
		C3D_TexEnvSrc(e, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
		C3D_TexEnvFunc(e, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(e, abgr(26, 26, 26, 255));
		tev_last.valid = false;
	}
	C3D_TexBind(0, &scene_tex[which]);
	int pw = game_w + 2 * gpu_margin;
	float u1 = (float) pw / SCENE_W, v1 = (float) game_h / SCENE_H;
	float x0 = off_x, y0 = off_y, x1 = off_x + pw * scale_x, y1 = off_y + game_h * scale_y;
	if (which == 0 && settings.smooth && scale_y < 0.9f) {
		/* smooth (settings: "Smooth pictures"): four samples a quarter of a
		 * screen pixel apart, averaged (blend 1, 1/2, 1/3, 1/4); each screen
		 * pixel then covers its ~1.7 game pixels instead of a bilinear
		 * sample skipping some (jagged edges on pictures and faces) */
		static const float d[4][2] = { { -0.25f, -0.25f }, { 0.25f, 0.25f }, { 0.25f, -0.25f }, { -0.25f, 0.25f } };
		float du = 1.0f / (scale_x * SCENE_W), dv = 1.0f / (scale_y * SCENE_H);
		for (int k = 0; k < 4; k++) {
			float ou = d[k][0] * du, ov = d[k][1] * dv;
			quad(x0, y0, x1, y1, ou, -ov, u1 + ou, v1 - ov, k ? 1.0f / (k + 1) : alpha, 0, 0, 0);
		}
		return;
	}
	quad(x0, y0, x1, y1, 0, 0, u1, v1, alpha, 0, 0, 0);
}

/* frozen_alpha: -1 just the picture; 0..254 the frozen picture over it
 * (transition); 255 just the frozen picture */
/* --- activity icons at the top right (settings: "Load/save icons"): a
 * spinning CD while something loads (and a moment after), a floppy disk
 * after something was written to the SD card */
static u64 act_load, act_write;
void gpu_activity_load(void) { act_load = svcGetSystemTick(); }
void gpu_activity_write(void) { act_write = svcGetSystemTick(); }

static void icon_px(Canvas *c, int x, int y, Color col)
{
	gpu_fill(c, (Rect) { x, y, 1, 1 }, &col, 255);
}

/* a 12x12 CD at (x0, y0), its shine turned by angle a */
static void draw_cd(Canvas *c, int x0, int y0, float a)
{
	float ca = cosf(a), sa = sinf(a);
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 12; x++) {
			float dx = x - 5.5f, dy = y - 5.5f, r = sqrtf(dx * dx + dy * dy);
			if (r > 6.0f || r < 1.5f)
				continue;
			Color col;
			if (r > 5.2f)
				col = (Color) { 90, 90, 100, 255 }; /* rim */
			else if (r < 2.5f)
				col = (Color) { 150, 150, 160, 255 }; /* around the hole */
			else {
				/* silver, with two shine sectors turning (cos of twice the angle) */
				float k = (dx * ca + dy * sa) / r, l = (dy * ca - dx * sa) / r;
				float shine = k * k - l * l; /* cos(2 * (angle - a)) */
				int v = shine > 0.6f ? 245 : shine > 0.0f ? 210 : 180;
				col = shine > 0.6f ? (Color) { 230, 240, 255, 255 } : (Color) { v, v, v + 10 > 255 ? 255 : v + 10, 255 };
			}
			icon_px(c, x0 + x, y0 + y, col);
		}
}

/* a 12x12 floppy disk at (x0, y0) */
static void draw_floppy(Canvas *c, int x0, int y0)
{
	static const char *const art[12] = {
		"BBBBBBBBBBB.",
		"BBMMMMMMBBBB",
		"BBMMMMDMBBBB",
		"BBMMMMDMBBBB",
		"BBMMMMMMBBBB",
		"BBBBBBBBBBBB",
		"BWWWWWWWWWWB",
		"BWkkkkkkkkWB",
		"BWWWWWWWWWWB",
		"BWkkkkkkkkWB",
		"BWWWWWWWWWWB",
		"BBBBBBBBBBBB",
	};
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 12; x++) {
			Color col;
			switch (art[y][x]) {
			case 'B': col = (Color) { 40, 70, 150, 255 }; break;  /* body */
			case 'M': col = (Color) { 190, 195, 205, 255 }; break; /* metal shutter */
			case 'D': col = (Color) { 60, 60, 70, 255 }; break;    /* its slot */
			case 'W': col = (Color) { 245, 245, 240, 255 }; break; /* label */
			case 'k': col = (Color) { 150, 150, 160, 255 }; break; /* label lines */
			default: continue;
			}
			icon_px(c, x0 + x, y0 + y, col);
		}
}

static void draw_activity(void)
{
	if (!settings.activity_icons)
		return;
	u64 now = svcGetSystemTick(), ms = SYSCLOCK_ARM11 / 1000;
	Canvas c = { .clip = { 0, 0, 400, 240 } };
	Color shade = { 0, 0, 0, 255 };
	int x = 400 - 3 - 12;
	if (act_load && now - act_load < 400 * ms) {
		gpu_fill(&c, (Rect) { x - 1, 2, 14, 14 }, &shade, 110);
		draw_cd(&c, x, 3, (float) (now / ms % 1000) * 6.2832f / 1000.0f * 2);
		x -= 16;
	}
	if (act_write && now - act_write < 1000 * ms) {
		gpu_fill(&c, (Rect) { x - 1, 2, 14, 14 }, &shade, 110);
		draw_floppy(&c, x, 3);
	}
}

/* the battery icon at the top left (settings: "Battery icon"): a 15x8
 * outline with a nub, filled by the charge; light blue while charging,
 * yellow below 30%, red below 15% */
static void draw_battery(void)
{
	int pct;
	bool charging;
	if (!settings.battery || !battery_get(&pct, &charging))
		return;
	Canvas c = { .clip = { 0, 0, 400, 240 } };
	Color shade = { 0, 0, 0, 255 }, line = { 235, 235, 235, 255 };
	Color fill = charging ? (Color) { 120, 200, 255, 255 } : pct < 15 ? (Color) { 255, 70, 60, 255 }
	           : pct < 30 ? (Color) { 255, 210, 60, 255 } : (Color) { 120, 230, 110, 255 };
	const int x = 3, y = 3, w = 15, h = 8;
	gpu_fill(&c, (Rect) { x - 1, y - 1, w + 4, h + 2 }, &shade, 110); /* (readable on bright pictures) */
	gpu_fill(&c, (Rect) { x, y, w, 1 }, &line, 255);
	gpu_fill(&c, (Rect) { x, y + h - 1, w, 1 }, &line, 255);
	gpu_fill(&c, (Rect) { x, y + 1, 1, h - 2 }, &line, 255);
	gpu_fill(&c, (Rect) { x + w - 1, y + 1, 1, h - 2 }, &line, 255);
	gpu_fill(&c, (Rect) { x + w, y + 2, 2, h - 4 }, &line, 255);
	int fw = (pct * (w - 4) + 50) / 100;
	if (fw < 1 && pct > 0)
		fw = 1;
	if (fw > 0)
		gpu_fill(&c, (Rect) { x + 2, y + 2, fw, h - 4 }, &fill, 255);
}

void gpu_present(int frozen_alpha, int brightness)
{
	C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x000000ff, 0);
	C3D_FrameDrawOn(top);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj_top);
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	presenting = true;
	tev_last.valid = false;
	cur_clip = (Rect) { -1, -1, -1, -1 };
	cur_blend = -1;
	if (frozen_alpha < 255) {
		present_tex(0, 1.0f);
		draw_text_jobs();
	}
	if (frozen_alpha >= 0)
		present_tex(1, frozen_alpha / 255.0f);
	if (brightness < 255) {
		Canvas c = { .clip = { 0, 0, top_w, 240 } };
		Color black = { 0, 0, 0, 255 };
		gpu_fill(&c, c.clip, &black, 255 - brightness);
	}
	/* (the engine's own things at 400 pixels across) */
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj_ui);
	draw_battery();
	draw_activity();
}

void gpu_frame_end(void)
{
	C3D_FrameEnd(0);
	in_frame = false;
	last_frame_end = svcGetSystemTick();
	/* the big images that waited: their textures now */
	for (int i = 0; i < npending; i++)
		texture_of(pending[i]);
	npending = 0;
}

void gpu_text_forget(Bitmap *b)
{
	for (int i = 0; i < ntext_jobs; i++)
		if (text_jobs[i].cont == b)
			text_jobs[i--] = text_jobs[--ntext_jobs];
	for (int i = 0; i < npending; i++)
		if (pending[i] == b)
			pending[i--] = pending[--npending];
}

/* the loading bar: the last picture again (with its window text) and a bar
 * at the top edge (frac < 0: a moving piece) */
unsigned gpu_loading_frames; /* (for the log) */
void gpu_loading_frame(int frozen_alpha, int brightness, float frac)
{
	if (!gpu_on || in_frame)
		return;
	gpu_loading_frames++;
	C3D_FrameBegin(0);
	in_frame = true;
	(void) frac;
	gpu_present(frozen_alpha, brightness); /* (with the CD / floppy icon) */
	C3D_FrameEnd(0);
	in_frame = false;
}

/* --- many quads from one bitmap (tilemaps): set up once, then gpu_tile */
static float tile_tw, tile_th, tile_alpha;

bool gpu_tiles_begin(Canvas *c, Bitmap *b, int opacity)
{
	GpuTex *g = texture_of(b);
	if (!g || g->nx * g->ny != 1)
		return false;
	if (g->repeat) {
		C3D_TexSetWrap(&g->tex[0], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		g->repeat = false;
	}
	set_clip(c->clip);
	set_blend(0);
	set_tev(true, NULL, c->tone);
	C3D_TexBind(0, &g->tex[0]);
	tile_tw = g->tex[0].width;
	tile_th = g->tex[0].height;
	tile_alpha = opacity / 255.0f;
	return true;
}

/* source rect in the bitmap (pixels) -> destination rect (game pixels);
 * half a pixel inside the source, so filtering doesn't bleed in neighbours */
void gpu_tile(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	float u0 = (sx + 0.5f) / tile_tw, u1 = (sx + sw - 0.5f) / tile_tw;
	float v0 = 1 - (sy + 0.5f) / tile_th, v1 = 1 - (sy + sh - 0.5f) / tile_th;
	quad(dx, dy, dx + dw, dy + dh, u0, v0, u1, v1, tile_alpha, 0, 0, 0);
}

/* --- vertex buffers (tilemaps) */
void *gpu_alloc(size_t bytes) { return linearAlloc(bytes); }
void gpu_free(void *p)
{
	if (p)
		linearFree(p);
}
void gpu_flush(const void *p, size_t bytes) { GSPGPU_FlushDataCache(p, bytes); }

void gpu_untextured_begin(Canvas *c)
{
	set_clip(c->clip);
	set_blend(0);
	set_tev(false, NULL, NULL);
}

/* draw vertices [first, first + count) as triangles, moved by (tx, ty) */
void gpu_draw_buffer(const float *verts, int first, int count, float tx, float ty)
{
	if (count <= 0)
		return;
	C3D_BufInfo *bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, verts, 9 * sizeof(float), 3, 0x210);
	C3D_Mtx m = proj_scene;
	Mtx_Translate(&m, tx, ty, 0, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &m);
	C3D_DrawArrays(GPU_TRIANGLES, first, count);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj_scene);
}

/* a Plane in one quad: the texture repeats by itself (only when the bitmap
 * is exactly its texture's size, i.e. power-of-two sides); false: not
 * possible, draw it in pieces */
bool gpu_plane(Canvas *c, Bitmap *b, int bx, int by, double zx, double zy, int opacity, int blend,
               const Color *color, const Tone *tone)
{
	GpuTex *g = texture_of(b);
	if (!g)
		return true;
	if (g->transparent || opacity <= 0)
		return true; /* nothing to see */
	if (g->nx * g->ny != 1 || g->tex[0].width != b->w || g->tex[0].height != b->h || zx <= 0 || zy <= 0)
		return false;
	if (!g->repeat) {
		C3D_TexSetWrap(&g->tex[0], GPU_REPEAT, GPU_REPEAT);
		g->repeat = true;
	}
	set_clip(c->clip);
	set_blend(blend);
	Tone merged;
	set_tev(true, color, merge_tone(tone, c->tone, &merged));
	C3D_TexBind(0, &g->tex[0]);
	/* the clip area; texture coordinates counted from the plane's origin */
	float x0 = c->clip.x, y0 = c->clip.y, x1 = c->clip.x + c->clip.w, y1 = c->clip.y + c->clip.h;
	float tw = b->w * (float) zx, th = b->h * (float) zy;
	float u0 = (x0 - bx) / tw, u1 = (x1 - bx) / tw;
	float v0 = 1 - (y0 - by) / th, v1 = 1 - (y1 - by) / th;
	quad(x0, y0, x1, y1, u0, v0, u1, v1, opacity / 255.0f, 0, 0, 0);
	return true;
}
