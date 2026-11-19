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
static C3D_Mtx proj_scene, proj_top;
static float scale, off_x, off_y; /* game -> top screen */
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

typedef struct {
	C3D_Tex tex;
	int w, h; /* bitmap size it was made for */
} GpuTex;

/* textures of disposed bitmaps: freed once the GPU is done with them */
static GpuTex *trash[256];
static int ntrash;

bool gpu_on;

void gpu_init(void)
{
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
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
	Mtx_OrthoTilt(&proj_top, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);
	gpu_on = true;
}

/* the game's screen size: how it fits onto the top screen */
void gpu_set_screen(int w, int h)
{
	if (!gpu_on)
		return;
	game_w = w;
	game_h = h;
	scale = w * 240 > h * 400 ? 400.0f / w : 240.0f / h;
	off_x = (400 - w * scale) / 2;
	off_y = (240 - h * scale) / 2;
}

static bool presenting;

static void set_clip(Rect r)
{
	if (presenting || !memcmp(&r, &cur_clip, sizeof r))
		return;
	cur_clip = r;
	int x0 = CLAMP(r.x, 0, SCENE_W), y0 = CLAMP(r.y, 0, SCENE_H);
	int x1 = CLAMP(r.x + r.w, 0, SCENE_W), y1 = CLAMP(r.y + r.h, 0, SCENE_H);
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
static void set_tev(bool textured, const Color *color, const Tone *tone)
{
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

/* the bitmap's texture, uploaded if the bitmap changed */
static GpuTex *texture_of(Bitmap *b)
{
	if (b->w > 1024 || b->h > 1024)
		return NULL;
	GpuTex *g = b->gpu;
	if (g && g->w == b->w && g->h == b->h && b->gpu_version == b->version + 1)
		return g;
	if (!g || g->w != b->w || g->h != b->h) {
		if (g) {
			if (ntrash < (int) (sizeof trash / sizeof *trash))
				trash[ntrash++] = g;
		}
		g = calloc(1, sizeof *g);
		if (!g || !C3D_TexInit(&g->tex, pow2(b->w), pow2(b->h), GPU_RGBA8)) {
			free(g);
			b->gpu = NULL;
			return NULL;
		}
		memset(g->tex.data, 0, g->tex.size);
		C3D_TexSetFilter(&g->tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&g->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		g->w = b->w;
		g->h = b->h;
		b->gpu = g;
	}
	/* RGBA8 in 8x8 tiles (Morton order inside); the GPU reads memory row r
	 * at v = 1 - r / height, so bitmap row y goes to row y */
	u32 *dst = g->tex.data;
	int tw = g->tex.width;
	for (int y = 0; y < b->h; y++) {
		int ty = y;
		const px_t *src = b->px + (size_t) y * b->w;
		u32 rowbase = (u32) ((ty >> 3) * (tw >> 3)) << 6;
		u32 my = ((ty & 1) << 1) | ((ty & 2) << 2) | ((ty & 4) << 3);
		for (int x = 0; x < b->w; x++) {
			u32 mx = (x & 1) | ((x & 2) << 1) | ((x & 4) << 2);
			dst[rowbase + ((u32) (x >> 3) << 6) + (mx | my)] = __builtin_bswap32(src[x]);
		}
	}
	C3D_TexFlush(&g->tex);
	b->gpu_version = b->version + 1;
	return g;
}

void gpu_forget(Bitmap *b)
{
	if (b->gpu) {
		if (gpu_on && ntrash < (int) (sizeof trash / sizeof *trash))
			trash[ntrash++] = b->gpu;
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

void gpu_draw(Canvas *c, Bitmap *src, Rect sr, const DrawOpts *o)
{
	if (sr.x < 0) { sr.w += sr.x; sr.x = 0; }
	if (sr.y < 0) { sr.h += sr.y; sr.y = 0; }
	if (sr.x + sr.w > src->w) sr.w = src->w - sr.x;
	if (sr.y + sr.h > src->h) sr.h = src->h - sr.y;
	if (sr.w <= 0 || sr.h <= 0 || o->opacity <= 0 || o->zx == 0 || o->zy == 0)
		return;
	GpuTex *g = texture_of(src);
	if (!g)
		return;
	set_clip(c->clip);
	set_blend(o->blend);
	Tone merged;
	set_tev(true, o->color, merge_tone(o->tone, c->tone, &merged));
	C3D_TexBind(0, &g->tex);
	float tw = g->tex.width, th = g->tex.height;
	float x0 = o->x, y0 = o->y, x1 = o->x + sr.w * (float) o->zx, y1 = o->y + sr.h * (float) o->zy;
	float u0 = sr.x / tw, u1 = (sr.x + sr.w) / tw;
	if (o->mirror) {
		float t = u0;
		u0 = u1;
		u1 = t;
	}
	float a = o->opacity / 255.0f;
	if (o->bush_depth > 0 && o->bush_depth < sr.h) {
		/* the lower bush_depth rows: less opaque */
		int split = sr.h - o->bush_depth;
		float ys = o->y + split * (float) o->zy;
		quad(x0, y0, x1, ys, u0, 1 - sr.y / th, u1, 1 - (sr.y + split) / th, a, (float) o->angle, o->cx, o->cy);
		quad(x0, ys, x1, y1, u0, 1 - (sr.y + split) / th, u1, 1 - (sr.y + sr.h) / th,
		     a * o->bush_opacity / 255.0f, (float) o->angle, o->cx, o->cy);
		return;
	}
	quad(x0, y0, x1, y1, u0, 1 - sr.y / th, u1, 1 - (sr.y + sr.h) / th, a, (float) o->angle, o->cx, o->cy);
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
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	/* the GPU is done with the last frame: free textures of disposed bitmaps */
	for (int i = 0; i < ntrash; i++) {
		C3D_TexDelete(&trash[i]->tex);
		free(trash[i]);
	}
	ntrash = 0;
}

/* draw the scene into texture 0 (the picture) or 1 (the frozen one) */
void gpu_scene_begin(int which)
{
	C3D_RenderTargetClear(scene_rt[which], C3D_CLEAR_ALL, 0x000000ff, 0);
	C3D_FrameDrawOn(scene_rt[which]);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj_scene);
	presenting = false;
	cur_clip = (Rect) { -1, -1, -1, -1 };
	cur_blend = -1;
}

/* a scene texture onto the top screen, scaled with filtering */
static void present_tex(int which, float alpha)
{
	set_blend(0);
	set_tev(true, NULL, NULL);
	C3D_TexBind(0, &scene_tex[which]);
	float u1 = (float) game_w / SCENE_W, v1 = (float) game_h / SCENE_H;
	quad(off_x, off_y, off_x + game_w * scale, off_y + game_h * scale, 0, 0, u1, v1, alpha, 0, 0, 0);
}

/* frozen_alpha: -1 just the picture; 0..254 the frozen picture over it
 * (transition); 255 just the frozen picture */
void gpu_present(int frozen_alpha, int brightness)
{
	C3D_RenderTargetClear(top, C3D_CLEAR_ALL, 0x000000ff, 0);
	C3D_FrameDrawOn(top);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_proj, &proj_top);
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	presenting = true;
	cur_clip = (Rect) { -1, -1, -1, -1 };
	cur_blend = -1;
	if (frozen_alpha < 255)
		present_tex(0, 1.0f);
	if (frozen_alpha >= 0)
		present_tex(1, frozen_alpha / 255.0f);
	if (brightness < 255) {
		Canvas c = { .clip = { 0, 0, 400, 240 } };
		Color black = { 0, 0, 0, 255 };
		gpu_fill(&c, c.clip, &black, 255 - brightness);
	}
}

void gpu_frame_end(void)
{
	C3D_FrameEnd(0);
}

/* --- many quads from one bitmap (tilemaps): set up once, then gpu_tile */
static float tile_tw, tile_th, tile_alpha;

bool gpu_tiles_begin(Canvas *c, Bitmap *b, int opacity)
{
	GpuTex *g = texture_of(b);
	if (!g)
		return false;
	set_clip(c->clip);
	set_blend(0);
	set_tev(true, NULL, c->tone);
	C3D_TexBind(0, &g->tex);
	tile_tw = g->tex.width;
	tile_th = g->tex.height;
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
