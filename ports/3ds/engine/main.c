/* MKXP-Z for the 3DS (experiment): starts Ruby, the RGSS classes and the
 * game's scripts. The game's folder: next to the .3dsx, else sdmc:/mkxpz.
 * The bottom screen shows messages (and errors). */
#include "engine.h"
#include <3ds.h>
#include <ruby/encoding.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "embedded.h" /* boot.rb and the RPG modules, made by build.sh */

VALUE eRGSSError, eRGSSReset;
int rgss_version = 3;

void fonts_scan(const char *engine_dir);

static FILE *log_file;

void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (log_file) {
		va_start(ap, fmt);
		vfprintf(log_file, fmt, ap);
		va_end(ap);
		fflush(log_file);
	}
}

static VALUE m_log(VALUE self, VALUE s)
{
	say("%s\n", StringValueCStr(s));
	return Qnil;
}

static bool has_game(const char *dir)
{
	char p[512];
	snprintf(p, sizeof p, "%s/Game.ini", dir);
	FILE *f = fopen(p, "r");
	if (f)
		fclose(f);
	return f != NULL;
}

/* Game.ini: the script file (Scripts=Data\Scripts.rvdata2) */
static void read_ini(char *scripts, size_t n)
{
	snprintf(scripts, n, "Data/Scripts.rvdata2");
	FILE *f = fopen("Game.ini", "r");
	if (!f)
		return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		if (strncasecmp(line, "Scripts=", 8))
			continue;
		char *v = line + 8;
		v[strcspn(v, "\r\n")] = 0;
		for (char *c = v; *c; c++)
			if (*c == '\\')
				*c = '/';
		snprintf(scripts, n, "%s", v);
	}
	fclose(f);
}

/* a file "gputest" in the game folder: known shapes for 5 seconds (checks
 * clipping and orientation of the scene texture) */
static void gpu_selftest(void)
{
	FILE *f = fopen("gputest", "r");
	if (!f)
		return;
	fclose(f);
	VALUE bv = bitmap_new_size(64, 64);
	rb_gc_register_address(&bv);
	Bitmap *b = bitmap_get(bv);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			b->px[y * 64 + x] = y < 16 ? PX(255, 255, 255, 255) : x < 16 ? PX(255, 0, 255, 255) : PX(0, 128, 255, 255);
	b->version++;
	for (int i = 0; i < 300 && aptMainLoop(); i++) {
		gpu_frame_begin();
		gpu_scene_begin(0);
		Canvas c = { .w = screen_w, .h = screen_h, .clip = { 0, 0, screen_w, screen_h }, .gpu = true };
		Color white = { 255, 255, 255, 255 }, red = { 255, 0, 0, 255 }, green = { 0, 255, 0, 255 };
		gpu_fill(&c, (Rect) { 0, 0, screen_w, screen_h }, &white, 255);
		/* red: the whole screen, clipped to the top left quarter */
		Canvas q = c;
		q.clip = (Rect) { 0, 0, screen_w / 2, screen_h / 2 };
		gpu_fill(&q, (Rect) { 0, 0, screen_w, screen_h }, &red, 255);
		/* green: bottom right corner, 100x80 */
		gpu_fill(&c, (Rect) { screen_w - 100, screen_h - 80, 100, 80 }, &green, 255);
		/* the bitmap (white band on top, magenta left) at 300,40, twice the size */
		DrawOpts o = { .x = 300, .y = 40, .zx = 2, .zy = 2, .opacity = 255 };
		canvas_draw(&c, b, (Rect) { 0, 0, 64, 64 }, &o);
		gpu_present(-1, 255);
		gpu_frame_end();
	}
	rb_gc_unregister_address(&bv);
}

static VALUE eval_named(VALUE arg)
{
	const char **a = (const char **) arg;
	return rb_funcall(rb_mKernel, rb_intern("eval"), 3, rb_str_new_cstr(a[0]),
	                  rb_const_get(rb_cObject, rb_intern("TOPLEVEL_BINDING")), rb_str_new_cstr(a[1]));
}

static bool eval_source(const char *src, const char *name)
{
	const char *a[2] = { src, name };
	int state = 0;
	rb_protect(eval_named, (VALUE) a, &state);
	return state == 0;
}

static VALUE run_scripts(VALUE unused)
{
	return rb_funcall(rb_cObject, rb_intern("mkxpz_run_scripts"), 0);
}

static void show_error(void)
{
	VALUE err = rb_errinfo();
	if (NIL_P(err))
		return;
	VALUE cls = rb_class_name(rb_obj_class(err));
	say("(%s)\n", StringValueCStr(cls));
	VALUE msg = rb_funcall(err, rb_intern("full_message"), 1, rb_hash_new());
	say("\n%s\n", StringValueCStr(msg));
}

static bool finished;

/* an unexpected exit (e.g. a Ruby "[BUG]"): keep the message on screen */
static void hold_on_exit(void)
{
	if (finished)
		return;
	say("\n*** unexpected exit ***\n");
	for (int i = 0; i < 60 * 30 && aptMainLoop(); i++)
		gspWaitForVBlank();
}

static void wait_start(void)
{
	say("\nPress START to quit.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	consoleInit(GFX_BOTTOM, NULL);
	atexit(hold_on_exit);
	romfsInit();
	osSetSpeedupEnable(true); /* New 3DS: 804 MHz */

	/* the game: next to the .3dsx, else sdmc:/mkxpz */
	char dir[512] = "sdmc:/mkxpz";
	if (argc > 0 && argv[0]) {
		snprintf(dir, sizeof dir, "%s", argv[0]);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = 0;
		if (!has_game(dir))
			snprintf(dir, sizeof dir, "sdmc:/mkxpz");
	}
	if (chdir(dir) != 0 || !has_game(".")) {
		printf("No game found: put it into sdmc:/mkxpz\n(with its Game.ini), or next to the .3dsx.\n");
		wait_start();
		gfxExit();
		return 1;
	}
	log_file = fopen("mkxpz-3ds.log", "w");
	char scripts[256];
	read_ini(scripts, sizeof scripts);
	const char *ext = strrchr(scripts, '.');
	rgss_version = ext && !strcasecmp(ext, ".rvdata2") ? 3 : ext && !strcasecmp(ext, ".rvdata") ? 2 : 1;
	say("MKXP-Z 3DS: %s (RGSS%d)\n", dir, rgss_version);

	ruby_sysinit(&argc, &argv);
	{
		RUBY_INIT_STACK;
		/* (ruby_init would exit without a word on failure) */
		if (ruby_setup()) {
			say("Ruby didn't start:\n");
			show_error();
			wait_start();
			return 1;
		}
		say("Ruby started\n");
		char *rargs[] = { "mkxp-z", "-e ", NULL };
		void *node = ruby_options(2, rargs);
		int st = 0;
		if (!ruby_executable_node(node, &st)) {
			say("Ruby didn't start (%d)\n", st);
			wait_start();
			return 1;
		}
		say("step: options\n");
		rb_enc_set_default_external(rb_enc_from_encoding(rb_utf8_encoding()));
		rb_gv_set("$MKXPZ_RGSS", INT2FIX(rgss_version));
		rb_gv_set("$MKXPZ_SCRIPTS", rb_str_new_cstr(scripts));
		rb_require("zlib");

		say("step: zlib\n");
		rb_define_global_function("mkxpz_log", m_log, 1);
		init_types();
		init_font();
		init_bitmap();
		init_scene();
		init_window();
		gpu_init();
		if (!audio_start())
			say("No sound: the DSP firmware is missing (run \"DSP1\" once: sdmc:/3ds/dspfirm.cdc)\n");
		init_graphics();
		init_input();
		init_audio();
		if (rgss_version >= 2)
			init_tilemap();
		say("step: classes\n");
		bool ok = eval_source(boot_rb, "boot.rb");
		say("step: boot.rb\n");
		eRGSSError = rb_const_get(rb_cObject, rb_intern("RGSSError"));
		eRGSSReset = rb_const_get(rb_cObject, rb_intern("RGSSReset"));
		const char *rpg = rgss_version == 3 ? module_rpg3_rb : rgss_version == 2 ? module_rpg2_rb : module_rpg1_rb;
		ok = ok && eval_source(rpg, "module_rpg.rb");
		say("step: rpg\n");
		fonts_scan("romfs:/fonts");
		say("step: fonts\n");
		if (!ok) {
			show_error();
			wait_start();
			return 1;
		}
		int state = 0;
		gpu_selftest();
		say("step: scripts\n");
		rb_protect(run_scripts, Qnil, &state);
		say("scripts ended (%d)\n", state);
		if (state) {
			show_error();
			wait_start();
		}
		audio_stop();
		ruby_cleanup(0);
	}
	finished = true;
	if (log_file)
		fclose(log_file);
	romfsExit();
	gfxExit();
	return 0;
}
