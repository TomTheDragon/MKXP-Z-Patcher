/* Graphics: the game's screen (in memory, at its own resolution), shown on
 * the 3DS's top screen (400x240), scaled to fit. */
#include "engine.h"
#include <math.h>
extern unsigned gpu_loading_frames, gpu_null_pending, gpu_null_alloc, gpu_null_px, gpu_evictions; /* gpu.c */
#include <3ds.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

int screen_w = 544, screen_h = 416;

static px_t *screen;          /* the composed frame */
static px_t *frozen;          /* Graphics.freeze: the frame shown until transition */
static int frame_rate = 60, frame_count, brightness = 255;
int graphics_frame(void) { return frame_count; }
static VALUE mGraphics;

/* scaling to the top screen: per output pixel, the source position (16.16) */
static int out_w, out_h, out_x, out_y;
static int *map_x, *map_y;

static void setup_scaling(void)
{
	free(map_x);
	free(map_y);
	/* fit inside 400x240, keeping the aspect ratio */
	if (screen_w * 240 > screen_h * 400) {
		out_w = 400;
		out_h = screen_h * 400 / screen_w;
	} else {
		out_h = 240;
		out_w = screen_w * 240 / screen_h;
	}
	out_x = (400 - out_w) / 2;
	out_y = (240 - out_h) / 2;
	map_x = malloc(sizeof *map_x * out_w);
	map_y = malloc(sizeof *map_y * out_h);
	for (int x = 0; x < out_w; x++)
		map_x[x] = (int) (((long long) x * 2 + 1) * screen_w * 65536 / (out_w * 2)) - 32768;
	for (int y = 0; y < out_h; y++)
		map_y[y] = (int) (((long long) y * 2 + 1) * screen_h * 65536 / (out_h * 2)) - 32768;
}

static void alloc_screen(int w, int h)
{
	px_t *s = calloc((size_t) w * h, 4);
	if (!s)
		rb_raise(rb_eNoMemError, "no memory for a %dx%d screen", w, h);
	free(screen);
	free(frozen);
	frozen = NULL;
	screen = s;
	screen_w = w;
	screen_h = h;
	setup_scaling();
	gpu_set_screen(w, h);
}

static void compose(px_t *dst)
{
	for (size_t i = 0, n = (size_t) screen_w * screen_h; i < n; i++)
		dst[i] = 0xff000000;
	Canvas c = { dst, screen_w, screen_h, screen_w, { 0, 0, screen_w, screen_h } };
	scene_draw(&c);
}

/* bilinear scaling into the top screen's framebuffer (240x400, rotated) */
static void present(const px_t *src)
{
	u16 fw, fh;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
	memset(fb, 0, (size_t) fw * fh * 3);
	int br = brightness;
	for (int ox = 0; ox < out_w; ox++) {
		int sx = map_x[ox], x0 = sx >> 16, fx = (sx >> 8) & 0xff;
		if (sx < 0) { x0 = 0; fx = 0; }
		int x1 = x0 + 1 < screen_w ? x0 + 1 : x0;
		u8 *col = fb + (size_t) (out_x + ox) * 240 * 3;
		for (int oy = 0; oy < out_h; oy++) {
			int sy = map_y[oy], y0 = sy >> 16, fy = (sy >> 8) & 0xff;
			if (sy < 0) { y0 = 0; fy = 0; }
			int y1 = y0 + 1 < screen_h ? y0 + 1 : y0;
			px_t a = src[y0 * screen_w + x0], b = src[y0 * screen_w + x1];
			px_t c = src[y1 * screen_w + x0], d = src[y1 * screen_w + x1];
			int w00 = (256 - fx) * (256 - fy), w01 = fx * (256 - fy), w10 = (256 - fx) * fy, w11 = fx * fy;
			int r = (PX_R(a) * w00 + PX_R(b) * w01 + PX_R(c) * w10 + PX_R(d) * w11) >> 16;
			int g = (PX_G(a) * w00 + PX_G(b) * w01 + PX_G(c) * w10 + PX_G(d) * w11) >> 16;
			int bl = (PX_B(a) * w00 + PX_B(b) * w01 + PX_B(c) * w10 + PX_B(d) * w11) >> 16;
			if (br < 255) {
				r = r * br / 255;
				g = g * br / 255;
				bl = bl * br / 255;
			}
			u8 *p = col + (size_t) (239 - (out_y + oy)) * 3;
			p[0] = bl;
			p[1] = g;
			p[2] = r;
		}
	}
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

/* one frame: input, the HOME menu, then the picture */
static void frame(const px_t *show)
{
	if (!aptMainLoop())
		rb_exit(0);
	input_poll();
	engine_check_reset();
	present(show);
	frame_count++;
}

/* timing, shown on the bottom screen every 60 frames (ms per frame) */
static u64 t_compose, t_present, t_script, t_last;
static int t_frames;

static double ms(u64 ticks) { return ticks * 1000.0 / SYSCLOCK_ARM11; }

/* free normal memory: what malloc hasn't taken from the heap yet, plus its
 * own free blocks */


/* the last speed line (for tests: Graphics.mkxpz_speed) */
extern unsigned gpu_cmd_splits;
static bool first_frame_done; /* (the start's loading bar until then) */
static char speed_line[400];
static VALUE g_speed(VALUE self) { return rb_str_new_cstr(speed_line); }

/* the GPU: the scene into texture 0 (or 1: Graphics.freeze), shown on the
 * top screen; frozen_alpha: see gpu_present */
static bool gpu_frozen;

static u64 t_draw, t_begin;
extern unsigned gpu_uploads, gpu_upload_px, gpu_draws, gpu_evictions;
unsigned gpu_used_kb(unsigned *count);
extern unsigned bitmap_drops, bitmap_reloads;
extern u64 gpu_upload_ticks;

static void gpu_draw_scene(int which)
{
	gpu_scene_begin(which);
	Canvas c = { .w = screen_w, .h = screen_h, .clip = { 0, 0, screen_w, screen_h }, .gpu = true };
	u64 t0 = svcGetSystemTick();
	scene_draw(&c);
	t_draw += svcGetSystemTick() - t0;
}

/* frame skip (settings: auto): when the game falls behind, frames aren't
 * drawn (and not waited for), up to 3 in a row, so it keeps its speed */
static u64 due;
static int skipped_row, t_skipped;

/* --- the speed display (settings: "Speed display"): game speed (game
 * frames a second against 60) and free memory, on the bottom screen's
 * first line, twice a second */
static unsigned overlay_frames; /* Graphics.update calls since the last text */
static u64 overlay_since;
static bool overlay_shown;

static void overlay_text(void)
{
	u64 now = svcGetSystemTick();
	if (!settings.overlay) {
		if (overlay_shown)
			status_line("");
		overlay_shown = false;
		return;
	}
	if (!overlay_since) {
		overlay_since = now;
		return;
	}
	double secs = (double) (now - overlay_since) / SYSCLOCK_ARM11;
	if (secs < 0.5)
		return;
	int pct = (int) (overlay_frames / secs / frame_rate * 100 + 0.5);
	if (pct > 100 && !input_speedup())
		pct = 100; /* (above only with the speed-up) */
	overlay_frames = 0;
	overlay_since = now;
	extern u32 __ctru_heap_size, __ctru_linear_heap_size;
	char text[80];
	/* memory in use (not free: "GFX 1.5/20" read as almost empty); GFX:
	 * linear memory (textures, sound) and VRAM (screens, textures) */
	double heap_used = (double) __ctru_heap_size - (heap_free() + mem_arena_free());
	double gfx_total = (double) __ctru_linear_heap_size + 6 * 1048576.0;
	double gfx_used = gfx_total - linearSpaceFree() - vramSpaceFree();
	snprintf(text, sizeof text, "Speed %3d%%|RAM %.1f/%lu MB|GFX %.1f/%.0f MB", pct, heap_used / 1048576.0,
	         (unsigned long) (__ctru_heap_size >> 20), gfx_used / 1048576.0, gfx_total / 1048576.0);
	status_line(text);
	overlay_shown = true;
}

/* the scene and the top screen: once, or (3D) once per eye with the layers
 * shifted: behind the screen (parallax), on it, in front (windows,
 * pictures); left eye: in front to the right, behind to the left */
/* 3D: the layers' shifts for an eye (0 left, 1 right; -1: none) */
static void eye_shifts(int eye)
{
	if (eye < 0) {
		stereo_shift[0] = stereo_shift[1] = stereo_shift[2] = 0;
		return;
	}
	float slider = osGet3DSliderState();
	int d = (int) (slider * STEREO_MAX + 0.5f); /* game pixels per eye, at the most STEREO_MAX */
	int sign = eye == 0 ? 1 : -1;
	stereo_shift[0] = -sign * d; /* behind */
	stereo_shift[1] = 0;
	stereo_shift[2] = sign * d;  /* in front */
}

static void draw_eyes(int frozen_alpha, bool stereo)
{
	if (!stereo) {
		gpu_present_eye(0, 0);
		if (frozen_alpha < 255)
			gpu_draw_scene(0);
		gpu_present(frozen_alpha, brightness);
		return;
	}
	for (int eye = 0; eye < 2; eye++) {
		eye_shifts(eye);
		/* the picture moved by less than a screen pixel, so what's in front
		 * lands on whole screen pixels as in 2D (moved by whole game
		 * pixels, ~0.58 screen pixels each: thin lines, e.g. the white
		 * edge of Changed-special's window frame, fell between two pixels
		 * and looked double in one eye); the background takes the rest */
		float s = stereo_shift[2] * gpu_scale_x();
		gpu_present_eye(eye, rintf(s) - s);
		if (frozen_alpha < 255)
			gpu_draw_scene(0);
		gpu_present(frozen_alpha, brightness);
	}
	eye_shifts(-1);
	gpu_present_eye(0, 0);
}

static void gpu_frame_with(int frozen_alpha)
{
	first_frame_done = true;
	if (!aptMainLoop())
		rb_exit(0);
	input_poll();
	engine_check_reset();
	const u64 period = SYSCLOCK_ARM11 / 60;
	u64 now = svcGetSystemTick();
	/* speed-up (L held): no waiting for the screen, and a picture at most
	 * 15 times a second (by the clock): all the rest of the time runs the
	 * game (as fast as its scripts allow); the pacing starts again
	 * afterwards */
	static u64 fast_drawn;
	if (input_speedup()) {
		due = 0;
		if (frozen_alpha < 0 && now - fast_drawn < SYSCLOCK_ARM11 / 15) {
			frame_count++;
			return;
		}
		fast_drawn = now;
		bool stereo = gpu_stereo_update();
		gpu_frame_begin_sync(false);
		draw_eyes(frozen_alpha, stereo);
		gpu_frame_end();
		frame_count++;
		return;
	}
	due = due ? due + period : now;
	if (now > due + 8 * period)
		due = now; /* far behind (loading...): start counting again */
	else if (now > due + (settings.frame_skip + 2) * period)
		due = now - period; /* the scripts themselves are too slow:
		                         * skipping can't catch up, so the game
		                         * runs a little slower instead */
	/* skipped (not drawn) when 2 frames late, at most frame_skip (settings:
	 * 1..5, 0 off) in a row: 1 keeps at least 30 pictures a second, more
	 * keep the game's speed in slower scenes with fewer pictures */
	if (settings.frame_skip && frozen_alpha < 0 && now > due + 2 * period && skipped_row < settings.frame_skip &&
	    !gpu_redraw_frames) {
		skipped_row++;
		t_skipped++;
		frame_count++;
		return;
	}
	skipped_row = 0;
	bool stereo = gpu_stereo_update();
	u64 tb = svcGetSystemTick();
	gpu_frame_begin_sync(now <= due + period / 4);
	t_begin += svcGetSystemTick() - tb;
	draw_eyes(frozen_alpha, stereo);
	gpu_frame_end();
	if (gpu_redraw_frames)
		gpu_redraw_frames--;
	frame_count++;
}

static void gpu_frame(void)
{
	gpu_frame_with(gpu_frozen ? 255 : -1);
}

/* Ruby's garbage collector: total ms and runs */
static size_t gc_ms(void)
{
	static VALUE key;
	if (!key)
		key = ID2SYM(rb_intern("time"));
	return rb_gc_stat(key);
}

/* games whose scripts come from files through a loader (no "Main"
 * section, e.g. Changed): boot.rb's preparations run at the first
 * Graphics.update / freeze / transition, when all scripts are loaded */
static bool setup_later;
static void late_setup(void)
{
	if (setup_later) {
		setup_later = false;
		rb_funcall(rb_cObject, rb_intern("mkxpz_late_setup"), 0);
	}
}
static VALUE g_setup_later(VALUE self)
{
	setup_later = true;
	return Qnil;
}

static VALUE g_update(VALUE self)
{
	late_setup();
	overlay_frames++;
	overlay_text();
	mem_tell_ruby();
	prof_graphics_begin();
	u64 t0 = svcGetSystemTick();
	if (t_last)
		t_script += t0 - t_last;
	if (gpu_on) {
		gpu_frame();
		t_compose += svcGetSystemTick() - t0;
	} else if (frozen) {
		frame(frozen);
	} else {
		compose(screen);
		u64 t1 = svcGetSystemTick();
		t_compose += t1 - t0;
		frame(screen);
		t_present += svcGetSystemTick() - t1;
	}
	t_last = svcGetSystemTick();
	static int prof_frames;
	if (prof_on && ++prof_frames == 600) {
		prof_report(prof_frames);
		prof_frames = 0;
	}
	static size_t gc_last, gc_runs_last;
	static unsigned bitmaps_last;
	size_t gc_now = gc_ms(), gc_runs = rb_gc_count();
	if (++t_frames == 60 && !settings.show_speed && !log_enabled()) {
		gc_last = gc_now;
		gc_runs_last = gc_runs;
		bitmaps_last = bitmaps_created;
		t_script = t_compose = t_present = t_draw = t_begin = 0;
		t_frames = t_skipped = 0;
		gpu_uploads = gpu_upload_px = gpu_draws = 0;
		gpu_upload_ticks = 0;
	} else if (t_frames == 60) {
		unsigned used_n;
		snprintf(speed_line, sizeof speed_line, "ms: ruby %.1f (gc %.1f, %u runs), draw %.1f, wait %.1f, other %.1f, skipped %d, "
		         "free %lu/%lu KB, freed tex %u, used %u KB in %u, dropped %u (+%u after upload), reloaded %u, images %u (%lu KB, fiber stacks %lu KB), stack %u KB, new bitmaps %u, loading bars %u, no texture %u/%u/%u (later/memory/pixels), evicted %u, cmd splits %u", ms(t_script) / 60,
		         (double) (gc_now - gc_last) / 60, (unsigned) (gc_runs - gc_runs_last), ms(t_draw) / 60, ms(t_begin) / 60,
		         (ms(t_compose) - ms(t_draw) - ms(t_begin)) / 60, t_skipped,
		         (unsigned long) (linearSpaceFree() / 1024), (unsigned long) (heap_free() / 1024), gpu_evictions,
		         gpu_used_kb(&used_n), used_n, bitmap_drops, bitmap_released, bitmap_reloads, mem_px_maps, (unsigned long) (mem_px_bytes / 1024), (unsigned long) (mem_stack_bytes / 1024), stack_used_kb(),
		         bitmaps_created - bitmaps_last, gpu_loading_frames, gpu_null_pending, gpu_null_alloc, gpu_null_px,
		         gpu_evictions, gpu_cmd_splits);
		bitmaps_last = bitmaps_created;
		gc_last = gc_now;
		gc_runs_last = gc_runs;
		static int lines;
		if (lines++ % 10 == 0)
			log_only("frame %d: %s\n", frame_count, speed_line);
		printf("ms: ruby %.1f, draw %.1f, wait %.1f, other %.1f\n", ms(t_script) / 60, ms(t_draw) / 60,
		       ms(t_begin) / 60, (ms(t_compose) - ms(t_draw) - ms(t_begin)) / 60);
		printf("  uploads %u (%u kpx, %.1f ms), quads %u, skipped %d\n  free: %lu KB textures/sound, %lu KB other, freed tex %u\n",
		       gpu_uploads, gpu_upload_px / 1000, ms(gpu_upload_ticks) / 60, gpu_draws / 60, t_skipped,
		       (unsigned long) (linearSpaceFree() / 1024), (unsigned long) (heap_free() / 1024), gpu_evictions);
		t_skipped = 0;
		gpu_uploads = gpu_upload_px = gpu_draws = 0;
		gpu_upload_ticks = 0;
		t_script = t_compose = t_present = t_draw = t_begin = 0;
		t_frames = 0;
	}
	prof_graphics_end();
	return Qnil;
}

/* the loading bar: called while something loads; after 150 ms without a
 * frame, an extra frame with the bar every 50 ms at most */
static u64 last_bar;
static void activity_frame(float frac);
void loading_tick(float frac)
{
	gpu_activity_load();
	activity_frame(frac);
}

void writing_tick(void)
{
	gpu_activity_write();
	activity_frame(-1);
}

/* after 150 ms without a frame: an extra one (the icons), every 50 ms */
static void activity_frame(float frac)
{
	if (!gpu_on || gpu_in_frame())
		return;
	u64 now = svcGetSystemTick(), ms_ = SYSCLOCK_ARM11 / 1000;
	if (now - gpu_last_frame_end() < 150 * ms_ || now - last_bar < 50 * ms_)
		return;
	last_bar = now;
	gpu_loading_frame(gpu_frozen ? 255 : -1, brightness, frac);
}
void log_slow_load(const char *what, const char *name, unsigned long long since_tick)
{
	if (!log_enabled())
		return;
	double ms = (double) (svcGetSystemTick() - since_tick) / (SYSCLOCK_ARM11 / 1000.0);
	if (ms >= 10)
		log_only("slow load: %s %s %.0f ms (frame %d)\n", what, name ? name : "?", ms, frame_count);
}

static VALUE g_loading(int argc, VALUE *argv, VALUE self)
{
	loading_tick(argc > 0 && !NIL_P(argv[0]) ? (float) num2dbl(argv[0]) : -1);
	return Qnil;
}

/* (boot.rb, on a reset: full brightness, no transition waiting) */
static VALUE g_reset(VALUE self)
{
	brightness = 255;
	gpu_frozen = false;
	if (frozen) {
		free(frozen);
		frozen = NULL;
	}
	/* one frame of the (now empty) scene: black with the load icon, not
	 * the last picture while the scripts load again (seconds in games that
	 * read many script files) */
	if (gpu_on && !gpu_in_frame()) {
		gpu_activity_load();
		gpu_frame_begin_sync(false);
		gpu_draw_scene(0);
		gpu_present(-1, brightness);
		gpu_frame_end();
	}
	return Qnil;
}

/* (boot.rb: how far message boxes reach beyond the game's screen) */
/* (the wide text box WIDE_INSET from the screen's edges: in 3D it's
 * shifted toward the viewer by up to STEREO_MAX and keeps its whole
 * outline; the same in 2D, so switching 3D doesn't change the box) */
static VALUE g_margin(VALUE self)
{
	if (!gpu_on || !settings.wide_text)
		return INT2FIX(0); /* (the sides only for 3D shifts) */
	/* fit: the screen's sides; extended: the wider screen around the
	 * game's own width (the box stayed 544 wide in Changed's 704) */
	int base = rgss_version >= 2 ? 544 : 640;
	int m = settings.screen == 2 && screen_w > base ? (screen_w - base) / 2 : gpu_margin;
	return INT2FIX(m > WIDE_INSET ? m - WIDE_INSET : m);
}

static VALUE g_wait(VALUE self, VALUE n)
{
	for (int i = num2int(n); i > 0; i--)
		g_update(self);
	return Qnil;
}

static VALUE g_freeze(VALUE self)
{
	late_setup();
	if (gpu_on) {
		if (!gpu_frozen) {
			/* (3D: one frozen picture per eye; with the screen's brightness,
			 * as RGSS's freeze: a fade to black stayed black until the
			 * transition) */
			gpu_frame_begin();
			bool eyes = gpu_stereo_on() && gpu_frozen_right_ready();
			eye_shifts(eyes ? 0 : -1);
			gpu_draw_scene(1);
			gpu_scene_darken(brightness);
			if (eyes) {
				eye_shifts(1);
				gpu_draw_scene(3);
				gpu_scene_darken(brightness);
				eye_shifts(-1);
			}
			gpu_set_frozen_eyes(eyes);
			gpu_frame_end();
			bitmap_text_layers_drop_full();
			gpu_frozen = true;
		}
		return Qnil;
	}
	if (!frozen) {
		frozen = malloc((size_t) screen_w * screen_h * 4);
		if (frozen)
			compose(frozen);
	}
	return Qnil;
}

static VALUE load_bitmap(VALUE name)
{
	return rb_class_new_instance(1, &name, cBitmap);
}

/* Graphics.transition(duration = 10, filename = "", vague = 40) */
static VALUE g_transition(int argc, VALUE *argv, VALUE self)
{
	late_setup();
	int dur = argc > 0 ? num2int(argv[0]) : (rgss_version >= 2 ? 10 : 8);
	/* as in RGSS (and mkxp-z): a transition ends a fade-out, so the scene
	 * after the game over screen isn't black */
	brightness = 255;
	if (gpu_on) {
		/* (transition pictures: a plain cross-fade for now) */
		if (!gpu_frozen)
			return Qnil;
		gpu_frozen = false;
		for (int i = 1; i <= dur; i++)
			gpu_frame_with(255 - i * 255 / (dur + 1));
		return Qnil;
	}
	if (!frozen)
		return Qnil;
	px_t *old = frozen;
	frozen = NULL;
	px_t *mix = malloc((size_t) screen_w * screen_h * 4);
	/* a transition picture: dark parts change first */
	Bitmap *mask = NULL;
	int vague = argc > 2 ? CLAMP(num2int(argv[2]), 1, 255) : 40;
	VALUE maskv = Qnil;
	if (argc > 1 && RB_TYPE_P(argv[1], T_STRING) && RSTRING_LEN(argv[1]) > 0) {
		int state = 0;
		maskv = rb_protect(load_bitmap, argv[1], &state);
		if (state) {
			rb_set_errinfo(Qnil);
			maskv = Qnil;
		}
		mask = bitmap_get_maybe(maskv);
		if (mask && !bitmap_px(mask))
			mask = NULL;
	}
	for (int i = 1; i <= dur && mix; i++) {
		compose(screen);
		int t = i * 255 / dur;
		size_t n = (size_t) screen_w * screen_h;
		for (size_t p = 0; p < n; p++) {
			int a = t;
			if (mask) {
				int x = (int) (p % screen_w) * mask->w / screen_w, y = (int) (p / screen_w) * mask->h / screen_h;
				int m = PX_R(mask->px[y * mask->w + x]);
				/* threshold sweeps from 0 to 255 + vague */
				a = (t * (255 + vague) / 255 - m) * 255 / vague;
				a = CLAMP(a, 0, 255);
			}
			px_t o = old[p], s = screen[p];
			mix[p] = PX(PX_R(o) + (PX_R(s) - (int) PX_R(o)) * a / 255,
			            PX_G(o) + (PX_G(s) - (int) PX_G(o)) * a / 255,
			            PX_B(o) + (PX_B(s) - (int) PX_B(o)) * a / 255, 255);
		}
		frame(mix);
	}
	free(mix);
	free(old);
	if (!NIL_P(maskv))
		rb_funcall(maskv, rb_intern("dispose"), 0);
	return Qnil;
}

static VALUE g_fade(int dur, int to)
{
	int from = brightness;
	for (int i = 1; i <= dur; i++) {
		brightness = from + (to - from) * i / dur;
		g_update(Qnil);
	}
	brightness = to;
	return Qnil;
}
static VALUE g_fadeout(VALUE self, VALUE d) { return g_fade(num2int(d), 0); }
static VALUE g_fadein(VALUE self, VALUE d) { return g_fade(num2int(d), 255); }

static VALUE g_snap(VALUE self)
{
	VALUE bv = bitmap_new_size(screen_w, screen_h);
	Bitmap *b = bitmap_get(bv);
	if (gpu_snapshot_begin()) {
		Canvas c = { .w = screen_w, .h = screen_h, .clip = { 0, 0, screen_w, screen_h }, .gpu = true };
		scene_draw(&c);
		gpu_snapshot_end(b->px, screen_w, screen_h);
		bitmap_text_layers_drop_full();
	} else {
		compose(b->px); /* (no GPU: the CPU, without the map) */
	}
	b->version++;
	return bv;
}

static VALUE g_resize(VALUE self, VALUE w, VALUE h)
{
	/* (up to 1024 across: the extended screen's wider map, 704) */
	int nw = CLAMP(num2int(w), 16, 1024), nh = CLAMP(num2int(h), 16, 480);
	if (nw != screen_w || nh != screen_h)
		alloc_screen(nw, nh);
	return Qnil;
}

/* the PC patcher's widescreen script (embedded, boot.rb): its mode 2 (a
 * wider view on maps) with the setting Screen: extended, else off */
static VALUE g_widescreen_mode(VALUE self) { return INT2FIX(settings.screen == 2 ? 2 : 0); }
static VALUE g_widescreen_resize(int argc, VALUE *argv, VALUE self)
{
	if (argc < 2)
		rb_raise(rb_eArgError, "widescreen_resize(width, height[, frame width])");
	return g_resize(self, argv[0], argv[1]);
}

static VALUE g_width(VALUE self) { return INT2NUM(screen_w); }
static VALUE g_height(VALUE self) { return INT2NUM(screen_h); }
static VALUE g_frame_rate(VALUE self) { return INT2NUM(frame_rate); }
static VALUE g_set_frame_rate(VALUE self, VALUE v) { frame_rate = CLAMP(num2int(v), 10, 120); return v; }
static VALUE g_frame_count(VALUE self) { return INT2NUM(frame_count); }
static VALUE g_set_frame_count(VALUE self, VALUE v) { frame_count = num2int(v); return v; }
static VALUE g_brightness(VALUE self) { return INT2NUM(brightness); }
static VALUE g_set_brightness(VALUE self, VALUE v) { brightness = CLAMP(num2int(v), 0, 255); return v; }
static VALUE g_nothing(int argc, VALUE *argv, VALUE self) { return Qnil; }

void gpu_tex_dump(void);
static VALUE g_tex_dump(VALUE self) { gpu_tex_dump(); return Qnil; }
static VALUE g_preload_begin(VALUE self) { gpu_preload_begin(); return Qnil; }
/* (boot.rb: while the game's scripts load, until the first picture) */
static VALUE g_startup(VALUE self, VALUE frac) { if (!first_frame_done) gpu_startup_progress((float) NUM2DBL(frac)); return Qnil; }

void init_graphics(void)
{
	alloc_screen(rgss_version >= 2 ? 544 : 640, rgss_version >= 2 ? 416 : 480);
	mGraphics = rb_define_module("Graphics");
	rb_define_module_function(mGraphics, "update", g_update, 0);
	rb_define_module_function(mGraphics, "mkxpz_setup_later", g_setup_later, 0);
	rb_define_module_function(mGraphics, "mkxpz_margin", g_margin, 0);
	rb_define_module_function(mGraphics, "mkxpz_tex_dump", g_tex_dump, 0);
	rb_define_module_function(mGraphics, "mkxpz_preload_begin", g_preload_begin, 0);
	rb_define_module_function(mGraphics, "mkxpz_startup", g_startup, 1);
	rb_define_module_function(mGraphics, "mkxpz_reset", g_reset, 0);
	rb_define_module_function(mGraphics, "mkxpz_loading", g_loading, -1);
	rb_define_module_function(mGraphics, "wait", g_wait, 1);
	rb_define_module_function(mGraphics, "freeze", g_freeze, 0);
	rb_define_module_function(mGraphics, "transition", g_transition, -1);
	rb_define_module_function(mGraphics, "fadeout", g_fadeout, 1);
	rb_define_module_function(mGraphics, "fadein", g_fadein, 1);
	rb_define_module_function(mGraphics, "snap_to_bitmap", g_snap, 0);
	rb_define_module_function(mGraphics, "resize_screen", g_resize, 2);
	rb_define_module_function(mGraphics, "widescreen_mode", g_widescreen_mode, 0);
	rb_define_module_function(mGraphics, "widescreen_resize", g_widescreen_resize, -1);
	rb_define_module_function(mGraphics, "frame_reset", g_nothing, -1);
	rb_define_module_function(mGraphics, "play_movie", g_nothing, -1);
	rb_define_module_function(mGraphics, "width", g_width, 0);
	rb_define_module_function(mGraphics, "height", g_height, 0);
	rb_define_module_function(mGraphics, "frame_rate", g_frame_rate, 0);
	rb_define_module_function(mGraphics, "frame_rate=", g_set_frame_rate, 1);
	rb_define_module_function(mGraphics, "frame_count", g_frame_count, 0);
	rb_define_module_function(mGraphics, "frame_count=", g_set_frame_count, 1);
	rb_define_module_function(mGraphics, "brightness", g_brightness, 0);
	rb_define_module_function(mGraphics, "brightness=", g_set_brightness, 1);
	rb_define_module_function(mGraphics, "mkxpz_speed", g_speed, 0);
}
