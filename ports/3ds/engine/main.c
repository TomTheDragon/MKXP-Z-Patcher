/* MKXP-Z for the 3DS (experiment): starts Ruby, the RGSS classes and the
 * game's scripts. The game's folder: next to the .3dsx, else sdmc:/mkxpz.
 * The bottom screen shows messages (and errors). */
#include "engine.h"
#include <3ds.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <unistd.h>
#include <ruby/encoding.h>
#include <sys/iosupport.h>
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

/* Memory split (libctru reads this at startup): graphics ("linear") memory
 * 20 MB, the rest for the normal heap. libctru's default caps the heap at
 * 24 MB and gives graphics up to 32 MB, but on an original 3DS (64 MB for
 * the game) Ruby, the scripts and the images' pixels need the heap more:
 * textures are freed and uploaded again when graphics memory runs low
 * (a frame needs at most ~11 MB on Dazed's biggest map). */
u32 __ctru_linear_heap_size = 20 * 1024 * 1024;

/* The main thread's stack (libctru's default is 32 KB): Ruby, the game's
 * scenes and e.g. Marshal.dump when saving all run on it; 22 KB were used
 * right after starting. Ruby knows its size (ruby-3ds.patch), so running
 * out is a SystemStackError rather than a crash. */
u32 __stacksize__ = 512 * 1024;

static FILE *log_file;
/* the bottom screen shows messages only with "Bottom log" on, or while
 * console_force is set (errors, the settings menu) */
bool console_force;
static bool console_visible(void) { return settings.show_speed || console_force; }

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

/* only into mkxpz-3ds.log (not the bottom screen) */
void log_only(const char *fmt, ...)
{
	if (!log_file)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(log_file, fmt, ap);
	va_end(ap);
	fflush(log_file);
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
	return rb_funcall(rb_cObject, rb_intern("mkxpz_run_all"), 0);
}

/* "Class: message" and the first lines of the backtrace */
static VALUE error_text(VALUE err)
{
	VALUE s = rb_str_dup(rb_class_name(rb_obj_class(err)));
	rb_str_cat_cstr(s, ": ");
	rb_str_append(s, rb_obj_as_string(rb_funcall(err, rb_intern("message"), 0)));
	VALUE bt = rb_funcall(err, rb_intern("backtrace"), 0);
	for (long i = 0; RB_TYPE_P(bt, T_ARRAY) && i < RARRAY_LEN(bt) && i < 8; i++) {
		rb_str_cat_cstr(s, "\n  ");
		rb_str_append(s, rb_obj_as_string(RARRAY_AREF(bt, i)));
	}
	return s;
}

static void show_error(void)
{
	VALUE err = rb_errinfo();
	if (NIL_P(err))
		return;
	int state = 0;
	VALUE text = rb_protect(error_text, err, &state);
	if (state || !RB_TYPE_P(text, T_STRING)) {
		rb_set_errinfo(Qnil);
		say("\n(an error; its message couldn't be read)\n");
		return;
	}
	/* (no NUL bytes for printf) */
	char *p = RSTRING_PTR(text);
	for (long i = 0; i < RSTRING_LEN(text); i++)
		if (!p[i])
			p[i] = '?';
	console_force = true; /* (errors show also with the bottom log off) */
	say("\n%.*s\n", (int) RSTRING_LEN(text), p);
}

static bool finished;

/* an unexpected exit (e.g. a Ruby "[BUG]"): keep the message on screen */
static void hold_on_exit(void)
{
	if (finished)
		return;
	console_force = true;
	say("\n*** unexpected exit ***\n");
	for (int i = 0; i < 60 * 30 && aptMainLoop(); i++)
		gspWaitForVBlank();
}

static void wait_start(void)
{
	console_force = true;
	say("\nPress START to quit.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gspWaitForVBlank();
	}
}

/* The main thread's stack (libctru puts __stacksize__ bytes at the start of
 * the heap): filled with a pattern at startup, so the log can say how much
 * of it was ever used. */
static volatile unsigned char *stack_bottom, *stack_top;

static __attribute__((noinline)) void stack_paint(void)
{
	extern char *fake_heap_start;
	extern u32 __stacksize__;
	volatile unsigned char here = 0;
	stack_top = (unsigned char *) fake_heap_start;
	stack_bottom = stack_top - __stacksize__;
	for (volatile u32 *p = (volatile u32 *) (stack_bottom + 64); (u32) p < (u32) &here - 256; p++)
		*p = 0xa5a5a5a5u;
}

/* (the deepest point only moves down: checked from there in 1 KB steps,
 * so it costs next to nothing) */
unsigned stack_used_kb(void)
{
	static volatile u32 *low;
	if (!stack_bottom)
		return 0;
	if (!low)
		low = (volatile u32 *) ((u32) stack_top & ~3u);
	for (;;) {
		volatile u32 *q = low - 256;
		if ((volatile unsigned char *) q < stack_bottom + 64)
			break;
		bool touched = false;
		for (volatile u32 *r = q; r < low && !touched; r++)
			touched = *r != 0xa5a5a5a5u;
		if (!touched)
			break;
		low = q;
	}
	return (unsigned) ((stack_top - (volatile unsigned char *) low) / 1024);
}

/* what goes to stderr (e.g. Ruby's "[FATAL] failed to allocate memory"
 * before it quits) also goes into mkxpz-3ds.log */
static devoptab_t err_tab;
static const devoptab_t *err_orig;

static ssize_t err_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	if (log_file) {
		fwrite(ptr, 1, len, log_file);
		fflush(log_file);
	}
	if (!console_visible())
		return (ssize_t) len;
	return err_orig && err_orig->write_r ? err_orig->write_r(r, fd, ptr, len) : (ssize_t) len;
}

/* Writes to the SD card. On the console a save hung: a write reported 0
 * bytes written, and Ruby (which then waits until the file is writable,
 * and our poll says files always are) tried again forever. So a write that
 * writes nothing is tried again here, then with the data copied into a new
 * buffer straight through the FS service; if nothing helps it fails with
 * EIO (Ruby raises, the game's save reports a failure). With
 * "mkxpz-savelog" in the game folder every write goes into the log. */
static devoptab_t sd_tab;
static const devoptab_t *sd_orig;
static bool sd_log;

/* libctru's archive file (archive_dev.c): the FS handle, flags, offset */
typedef struct { Handle h; u32 flags; u64 offset; } ArchiveFile;

static void *log_fd;   /* the log's own writes pass straight through */
static bool in_sd_write;

static ssize_t sd_write_checked(struct _reent *r, void *fd, const char *ptr, size_t len);

static ssize_t sd_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
	if (pack_is(fd))
		return pack_read(r, fd, ptr, len);
	return sd_orig->read_r(r, fd, ptr, len);
}

static off_t sd_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
	if (pack_is(fd))
		return pack_seek(r, fd, pos, dir);
	return sd_orig->seek_r(r, fd, pos, dir);
}

static ssize_t sd_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	if (pack_is(fd)) {
		r->_errno = EBADF;
		return -1;
	}
	if (fd == log_fd || in_sd_write)
		return sd_orig->write_r(r, fd, ptr, len);
	in_sd_write = true;
	ssize_t n = sd_write_checked(r, fd, ptr, len);
	in_sd_write = false;
	writing_tick(); /* (the floppy icon) */
	return n;
}

static ssize_t sd_write_checked(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	ArchiveFile *f = fd;
	u64 at = f->offset;
	ssize_t n = sd_orig->write_r(r, fd, ptr, len);
	if (sd_log || n != (ssize_t) len)
		log_only("sd write: %u bytes at %llu from %p: %d%s\n", (unsigned) len, (unsigned long long) at, ptr, (int) n,
		         n < 0 ? " (error)" : "");
	if (n != 0 || len == 0)
		return n;
	/* nothing written: again, then from a copy straight through FS */
	n = sd_orig->write_r(r, fd, ptr, len);
	log_only("sd write: again: %d\n", (int) n);
	if (n != 0)
		return n;
	void *copy = memalign(64, len);
	if (copy) {
		memcpy(copy, ptr, len);
		u32 bytes = 0;
		Result rc = FSFILE_Write(f->h, &bytes, f->offset, copy, len, FS_WRITE_FLUSH);
		log_only("sd write: FSFILE_Write from a copy: result %08lx, %lu bytes\n", (unsigned long) rc, (unsigned long) bytes);
		free(copy);
		if (R_SUCCEEDED(rc) && bytes > 0) {
			f->offset += bytes;
			return bytes;
		}
	}
	r->_errno = EIO;
	return -1;
}

/* File times: libctru's stat / fstat on sdmc: leave st_mtime at 0 (1970
 * for every file), so a save screen's "most recent save" (Changed: by
 * file.mtime) always picked the first slot. Filled in here from
 * archive_getmtime; for fstat the path is noted when the file is opened. */
static struct { void *fd; char path[256]; } sd_paths[32];

static void sd_set_mtime(const char *path, struct stat *st)
{
	u64 t = 0;
	if (!path || R_FAILED(archive_getmtime(path, &t)) || !t)
		return;
	static bool told;
	if (!told) {
		told = true;
		log_only("file times: archive_getmtime(%s) = %llu\n", path, (unsigned long long) t);
	}
	st->st_mtime = (time_t) t;
}

static int sd_open(struct _reent *r, void *fileStruct, const char *path, int flags, int mode)
{
	int rc = sd_orig->open_r(r, fileStruct, path, flags, mode);
	if (rc == -1 && pack_open(r, fileStruct, path, flags) == 0)
		return 0; /* (from Game.mkxpz) */
	if (rc != -1) {
		for (int i = 0; i < 32; i++)
			if (!sd_paths[i].fd) {
				sd_paths[i].fd = fileStruct;
				snprintf(sd_paths[i].path, sizeof sd_paths[i].path, "%s", path);
				break;
			}
	}
	return rc;
}

static int sd_close(struct _reent *r, void *fd)
{
	if (pack_is(fd))
		return pack_close(fd);
	for (int i = 0; i < 32; i++)
		if (sd_paths[i].fd == fd)
			sd_paths[i].fd = NULL;
	return sd_orig->close_r(r, fd);
}

static int sd_stat(struct _reent *r, const char *file, struct stat *st)
{
	int rc = sd_orig->stat_r(r, file, st);
	if (rc != 0 && pack_stat(file, st) == 0) {
		r->_errno = 0;
		return 0;
	}
	if (rc == 0 && !st->st_mtime)
		sd_set_mtime(file, st);
	return rc;
}

static int sd_fstat(struct _reent *r, void *fd, struct stat *st)
{
	if (pack_is(fd))
		return pack_fstat(fd, st);
	int rc = sd_orig->fstat_r(r, fd, st);
	if (rc == 0 && !st->st_mtime)
		for (int i = 0; i < 32; i++)
			if (sd_paths[i].fd == fd) {
				sd_set_mtime(sd_paths[i].path, st);
				break;
			}
	return rc;
}

static void guard_sd_writes(void)
{
	int i = FindDevice("sdmc:");
	if (i < 0 || !devoptab_list[i] || !devoptab_list[i]->write_r)
		return;
	sd_orig = devoptab_list[i];
	sd_tab = *sd_orig;
	sd_tab.write_r = sd_write;
	if (sd_orig->read_r)
		sd_tab.read_r = sd_read;
	if (sd_orig->seek_r)
		sd_tab.seek_r = sd_seek;
	if (sd_orig->open_r && sd_orig->close_r) {
		sd_tab.open_r = sd_open;
		sd_tab.close_r = sd_close;
	}
	if (sd_orig->stat_r)
		sd_tab.stat_r = sd_stat;
	if (sd_orig->fstat_r)
		sd_tab.fstat_r = sd_fstat;
	devoptab_list[i] = &sd_tab;
	if (log_file) {
		__handle *h = __get_handle(fileno(log_file));
		log_fd = h ? h->fileStruct : NULL;
	}
	FILE *f = fopen("mkxpz-savelog", "r");
	if (f) {
		fclose(f);
		sd_log = true;
	}
}

bool log_enabled(void) { return log_file != NULL; }

/* the settings' "Log file" switched while the game runs */
void log_set_enabled(bool on)
{
	if (on && !log_file) {
		log_file = fopen("mkxpz-3ds.log", "a");
		if (log_file) {
			__handle *h = __get_handle(fileno(log_file));
			log_fd = h ? h->fileStruct : NULL;
		}
	} else if (!on && log_file) {
		FILE *f = log_file;
		log_file = NULL;
		log_fd = NULL;
		fclose(f);
	}
}

static void tee_stderr(void)
{
	err_orig = devoptab_list[STD_ERR];
	if (err_orig)
		err_tab = *err_orig;
	err_tab.name = "stderr";
	err_tab.write_r = err_write;
	devoptab_list[STD_ERR] = &err_tab;
}

/* The bottom screen: its last line is the speed display (status_line),
 * the rest the messages (they scroll above it) */
static PrintConsole con_log;

static void guard_status_line(void);
#define STATUS_ROWS 3 /* console rows the speed display covers */
static void console_init(void)
{
	consoleInit(GFX_BOTTOM, &con_log);
	/* the speed display at the top (status_draw), an empty row, then the
	 * messages (libctru's scrolling of a console window also wiped the line
	 * next to it: the empty row, and the display drawn again after output) */
	consoleSetWindow(&con_log, 0, STATUS_ROWS + 2, 40, 30 - STATUS_ROWS - 2);
	consoleSelect(&con_log);
	guard_status_line();
}

static char status_text[96];
static bool in_status;

static const devoptab_t *out_orig;

/* the speed display: the bottom screen's top 24 pixels, drawn straight
 * into its framebuffer with the console's font: "Speed 100%" twice as big,
 * the parts after it ("|" between them) at normal size beside it, one per
 * line. (status_text "": the area cleared.) */
/* drawn into a buffer of its own (the area column by column, as in the
 * framebuffer) and copied over only where it differs: drawing straight onto
 * the screen showed the cleared bar for a moment, many times while the
 * settings menu printed its lines (flicker) */
#define STATUS_H (STATUS_ROWS * 8)
static u16 status_px[320 * STATUS_H];
static bool status_dirty = true;

static void status_pixel(int x, int y, u16 c)
{
	if (x >= 0 && x < 320 && y >= 0 && y < STATUS_H)
		status_px[x * STATUS_H + (STATUS_H - 1 - y)] = c;
}

static void status_text_at(int x, int y, const char *t, int len, int scale, u16 fg)
{
	const ConsoleFont *f = &con_log.font;
	for (int i = 0; i < len; i++, x += 8 * scale) {
		int ch = (unsigned char) t[i] - f->asciiOffset;
		if (ch < 0 || ch >= f->numChars)
			continue;
		const u8 *g = f->gfx + 8 * ch;
		for (int r = 0; r < 8; r++)
			for (int b = 0; b < 8; b++)
				if (g[r] & (0x80 >> b))
					for (int sy = 0; sy < scale; sy++)
						for (int sx = 0; sx < scale; sx++)
							status_pixel(x + b * scale + sx, y + r * scale + sy, fg);
	}
}

static void status_render(void)
{
	const u16 back = 0x18e3, white = 0xffff, grey = 0xbdf7; /* RGB565 */
	bool on = status_text[0] != 0;
	for (int i = 0; i < 320 * STATUS_H; i++)
		status_px[i] = on ? back : 0;
	if (on) {
		const char *bar = strchr(status_text, '|');
		int n = bar ? (int) (bar - status_text) : (int) strlen(status_text);
		status_text_at(4, 4, status_text, n, 2, white);
		int y = 4;
		while (bar && y < STATUS_H) {
			const char *p = bar + 1;
			bar = strchr(p, '|');
			int m = bar ? (int) (bar - p) : (int) strlen(p);
			status_text_at(320 - 4 - 8 * m, y, p, m, 1, grey);
			y += 9;
		}
	}
	status_dirty = false;
}

static void status_draw(void)
{
	u16 *fb = (u16 *) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	if (!fb)
		return;
	in_status = true;
	if (status_dirty)
		status_render();
	/* (the area: the last STATUS_H pixels of each 240-pixel column) */
	for (int x = 0; x < 320; x++) {
		u16 *col = fb + x * 240 + (240 - STATUS_H);
		const u16 *src = status_px + x * STATUS_H;
		if (memcmp(col, src, STATUS_H * 2))
			memcpy(col, src, STATUS_H * 2);
	}
	in_status = false;
}

/* --- the game's icon in the background of the bottom screen: the 48x48
 * icon of the .3dsx's SMDH (make-package.py puts the game's there), 4x,
 * dimmed to 30%, below the speed display. Drawn only into pixels that are
 * dark (the console's background; its text is brighter), again after each
 * output: so it comes back where text scrolled away or was cleared. */
#define BG_SCALE 4
#define BG_X ((320 - 48 * BG_SCALE) / 2)
#define BG_Y (STATUS_H + (240 - STATUS_H - 48 * BG_SCALE) / 2)
static u16 *bg_icon; /* 48x48 RGB565, dimmed; NULL: none */
static char self_path[512];

static void bg_icon_load(void)
{
	if (!self_path[0]) {
		/* (no argv, e.g. in an emulator: a .3dsx in the game folder) */
		DIR *d = opendir(".");
		struct dirent *e;
		while (d && (e = readdir(d))) {
			size_t l = strlen(e->d_name);
			if (l > 5 && !strcasecmp(e->d_name + l - 5, ".3dsx")) {
				snprintf(self_path, sizeof self_path, "%s", e->d_name);
				break;
			}
		}
		if (d)
			closedir(d);
	}
	FILE *f = self_path[0] ? fopen(self_path, "rb") : NULL;
	if (!f)
		return;
	unsigned char head[44];
	u16 *icon = NULL;
	if (fread(head, 1, 44, f) == 44 && !memcmp(head, "3DSX", 4) && (head[4] | head[5] << 8) >= 44) {
		u32 at = head[32] | head[33] << 8 | head[34] << 16 | (u32) head[35] << 24;
		u32 size = head[36] | head[37] << 8 | head[38] << 16 | (u32) head[39] << 24;
		static u16 tiled[48 * 48];
		if (size >= 0x24C0 + sizeof tiled && !fseek(f, (long) at + 0x24C0, SEEK_SET) &&
		    fread(tiled, 2, 48 * 48, f) == 48 * 48 && (icon = malloc(48 * 48 * 2))) {
			for (int y = 0; y < 48; y++)
				for (int x = 0; x < 48; x++) {
					/* 8x8 tiles, Morton order inside */
					int m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
					u16 p = tiled[((y >> 3) * 6 + (x >> 3)) * 64 + m];
					int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
					r = r * 3 / 10, g = g * 3 / 10, b = b * 3 / 10;
					icon[y * 48 + x] = (u16) (r << 11 | g << 5 | b);
				}
		}
	}
	fclose(f);
	bg_icon = icon;
	log_only("bottom screen icon: %s\n", icon ? "from the .3dsx" : "none");
}

/* dark enough to be background (the dimmed icon is at most 30%) */
static bool bg_dark(u16 p)
{
	return ((p >> 11) & 31) <= 10 && ((p >> 5) & 63) <= 20 && (p & 31) <= 10;
}

static void bg_icon_draw(void)
{
	if (!bg_icon)
		return;
	u16 *fb = (u16 *) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	if (!fb)
		return;
	for (int sx = 0; sx < 48 * BG_SCALE; sx++) {
		int x = BG_X + sx;
		u16 *col = fb + x * 240;
		const u16 *src = bg_icon + sx / BG_SCALE;
		for (int sy = 0; sy < 48 * BG_SCALE; sy++) {
			int y = BG_Y + sy;
			u16 *d = col + (239 - y);
			u16 want = src[(sy / BG_SCALE) * 48];
			if (*d != want && bg_dark(*d))
				*d = want;
		}
	}
}

void status_line(const char *text)
{
	if (!strcmp(status_text, text ? text : "")) {
		status_draw();
		return;
	}
	snprintf(status_text, sizeof status_text, "%s", text ? text : "");
	status_dirty = true;
	status_draw();
}

/* every other output to the bottom screen is followed by the status line
 * again (libctru's scrolling of the message window also wiped the line
 * next to it, and the speed display flickered) */
static devoptab_t out_tab;

static ssize_t out_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	if (!in_status && !console_visible())
		return (ssize_t) len; /* (bottom log off) */
	ssize_t n = out_orig->write_r ? out_orig->write_r(r, fd, ptr, len) : (ssize_t) len;
	if (!in_status && status_text[0])
		status_draw();
	if (!in_status)
		bg_icon_draw();
	return n;
}

static void guard_status_line(void)
{
	out_orig = devoptab_list[STD_OUT];
	if (!out_orig || !out_orig->write_r)
		return;
	out_tab = *out_orig;
	out_tab.write_r = out_write;
	devoptab_list[STD_OUT] = &out_tab;
}

int main(int argc, char **argv)
{
	stack_paint();
	gfxInitDefault();
	console_init();
	atexit(hold_on_exit);
	romfsInit();
	battery_init();
	osSetSpeedupEnable(true); /* New 3DS: 804 MHz */

	/* the game: next to the .3dsx, else sdmc:/mkxpz */
	char dir[512] = "sdmc:/mkxpz";
	if (argc > 0 && argv[0]) {
		snprintf(self_path, sizeof self_path, "%s", argv[0]);
		snprintf(dir, sizeof dir, "%s", argv[0]);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = 0;
		if (!has_game(dir))
			snprintf(dir, sizeof dir, "sdmc:/mkxpz");
	}
	if (chdir(dir) != 0 || !has_game(".")) {
		console_force = true;
		printf("No game found: put it into sdmc:/mkxpz\n(with its Game.ini), or next to the .3dsx.\n");
		wait_start();
		gfxExit();
		return 1;
	}
	settings_load();
	/* the log file: settings "Log file", or a file mkxpz-log (tests) */
	FILE *force_log = fopen("mkxpz-log", "r");
	if (force_log)
		fclose(force_log);
	if (settings.log_file || force_log)
		log_file = fopen("mkxpz-3ds.log", "w");
	tee_stderr();
	guard_sd_writes();
	log_only("started as %s\n", self_path[0] ? self_path : "(no path)");
	pack_init(sd_orig);
	bg_icon_load();
	bg_icon_draw();
	char scripts[256];
	read_ini(scripts, sizeof scripts);
	const char *ext = strrchr(scripts, '.');
	rgss_version = ext && !strcasecmp(ext, ".rvdata2") ? 3 : ext && !strcasecmp(ext, ".rvdata") ? 2 : 1;
	say("MKXP-Z 3DS: %s (RGSS%d)\n", dir, rgss_version);

	/* Ruby's garbage collector: by default it lets 16 MB be allocated before
	 * collecting, nearly all of the ~19 MB heap here (with the image pixels
	 * counted too, see memory.c, garbage piled up until a save ran out of
	 * memory on the console) */
	setenv("RUBY_GC_MALLOC_LIMIT", "4000000", 0);
	setenv("RUBY_GC_MALLOC_LIMIT_MAX", "8000000", 0);
	setenv("RUBY_GC_OLDMALLOC_LIMIT", "4000000", 0);
	setenv("RUBY_GC_OLDMALLOC_LIMIT_MAX", "8000000", 0);
	/* fibers (event scripts, message boxes): 128 KB of C stack each instead
	 * of 256 KB (Ruby's minimum on 32-bit), with the 64 KB Ruby stack */
	setenv("RUBY_FIBER_MACHINE_STACK_SIZE", "131072", 0);
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
		init_fast();
		init_events();
		init_window();
		gpu_init();
		{
			extern u32 __ctru_heap_size, __ctru_linear_heap_size;
			bool n3ds = false;
			APT_CheckNew3DS(&n3ds);
			s64 appmem = 0;
			svcGetSystemInfo(&appmem, 0, 1); /* the application memory region */
			log_only("memory: %s, application %lu KB, heap %lu KB, linear %lu KB (free %lu KB), VRAM free %lu KB\n",
			         n3ds ? "New 3DS" : "3DS", (unsigned long) (appmem / 1024), (unsigned long) (__ctru_heap_size / 1024),
			         (unsigned long) (__ctru_linear_heap_size / 1024), (unsigned long) (linearSpaceFree() / 1024),
			         (unsigned long) (vramSpaceFree() / 1024));
		}
		/* (test: the emulator gives 96 MB; a real original 3DS 64 MB, which
		 * leaves about 35 MB of normal memory next to the 20 MB of graphics
		 * memory) */
		FILE *lm = fopen("mkxpz-lowmem", "r");
		if (lm) {
			fclose(lm);
			/* the heap as big as an original 3DS's (38 MB) */
			extern u32 __ctru_heap_size;
			size_t keep = 38060 * 1024;
			static void *volatile hog; /* (volatile: kept by the compiler) */
			hog = __ctru_heap_size > keep ? malloc(__ctru_heap_size - keep) : NULL;
			if (hog)
				say("test: normal memory reduced to %lu KB\n", (unsigned long) (heap_free() / 1024));
		}
		mem_init();
		if (!audio_start())
			say("No sound: the DSP firmware is missing (run \"DSP1\" once: sdmc:/3ds/dspfirm.cdc)\n");
		init_graphics();
		init_input();
		init_audio();
		if (rgss_version >= 2)
			init_tilemap();
		prof_init();
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
	/* (romfs stays mounted: FreeType keeps the built-in fonts open, and
	 * the C library closes all open files after main returns; with romfs
	 * gone that crashed on quitting with START after an error) */
	gfxExit();
	return 0;
}
