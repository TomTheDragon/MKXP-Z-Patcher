/* Input: the 3DS's buttons as RPG Maker's keys.
 *   default: A: C (confirm, action)   B: B (cancel, menu)   Y: A (Shift, run)
 *            X: L (page up)   R: R (page down)   L: speed-up (held)
 *            ZL / ZR (New 3DS): X / Y keys
 *   settings "Buttons", the old layout: B confirm, A cancel, Y run, X: X,
 *            L, R: L, R (no speed-up)
 *   D-pad / circle pad: directions
 *   START: the engine's settings   SELECT: reset the game (like F12) */
#include "engine.h"
#include <3ds.h>
#include <string.h>

enum { K_DOWN = 2, K_LEFT = 4, K_RIGHT = 6, K_UP = 8, K_A = 11, K_B = 12, K_C = 13, K_X = 14, K_Y = 15, K_Z = 16,
       K_L = 17, K_R = 18, K_SHIFT = 21, K_CTRL = 22, K_ALT = 23, K_F5 = 25, K_F6 = 26, K_F7 = 27, K_F8 = 28,
       K_F9 = 29, K_MAX = 32 };

static u32 now, before;
static int held_frames[K_MAX];
static bool reset_requested;

static u32 keys_for(int k)
{
	switch (k) {
	case K_DOWN: return KEY_DOWN; /* D-pad and circle pad */
	case K_LEFT: return KEY_LEFT;
	case K_RIGHT: return KEY_RIGHT;
	case K_UP: return KEY_UP;
	case K_C: return settings.buttons ? KEY_B : KEY_A;
	case K_B: return settings.buttons ? KEY_A : KEY_B;
	case K_A: case K_SHIFT: return KEY_Y;
	case K_X: return settings.buttons ? KEY_X : KEY_ZL;
	case K_Y: return settings.buttons ? 0 : KEY_ZR;
	case K_L: return settings.buttons ? KEY_L | KEY_ZL : KEY_X;
	case K_R: return settings.buttons ? KEY_R | KEY_ZR : KEY_R;
	default: return 0;
	}
}

/* R held for 2 seconds: reset (a bar fills up on the top screen, shown
 * after the first 0.3 s so ordinary presses don't flash it); R has to be
 * let go of before it can do it again */
#define RESET_HOLD_MS 2000
#define RESET_SHOW_MS 300
static u64 r_since;
static bool r_armed = true;

float input_reset_hold(void)
{
	if (!r_since)
		return 0;
	u64 ms = (svcGetSystemTick() - r_since) / (SYSCLOCK_ARM11 / 1000);
	if (ms < RESET_SHOW_MS)
		return 0;
	return ms >= RESET_HOLD_MS ? 1.0f : (float) ms / RESET_HOLD_MS;
}

void input_poll(void)
{
	hidScanInput();
	before = now;
	now = hidKeysHeld();
	if ((now & KEY_SELECT) && !(before & KEY_SELECT))
		reset_requested = true;
	if (!(now & KEY_R)) {
		r_since = 0;
		r_armed = true;
	} else if (r_armed) {
		if (!r_since)
			r_since = svcGetSystemTick();
		else if (svcGetSystemTick() - r_since >= (u64) RESET_HOLD_MS * (SYSCLOCK_ARM11 / 1000)) {
			reset_requested = true;
			r_since = 0;
			r_armed = false;
		}
	}
	if ((now & KEY_START) && !(before & KEY_START)) {
		if (settings_menu())
			reset_requested = true;
		now = hidKeysHeld();
		before = now; /* (buttons still held after the menu: no presses) */
		for (int k = 0; k < K_MAX; k++)
			held_frames[k] = keys_for(k) && (now & keys_for(k)) ? 2 : 0;
		return;
	}
	for (int k = 0; k < K_MAX; k++) {
		u32 m = keys_for(k);
		held_frames[k] = m && (now & m) ? held_frames[k] + 1 : 0;
	}
}

/* (movie.c) B pressed since the last poll: the movie is skipped */
bool input_skip_movie(void)
{
	return (now & KEY_B) && !(before & KEY_B);
}

/* L held (default layout): the game runs as fast as it can */
bool input_speedup(void)
{
	return !settings.buttons && (now & KEY_L);
}

void engine_check_reset(void)
{
	if (reset_requested) {
		reset_requested = false;
		rb_raise(eRGSSReset, "reset");
	}
}

static int key_of(VALUE v)
{
	if (SYMBOL_P(v)) {
		static const struct { const char *name; int k; } names[] = {
			{ "DOWN", K_DOWN }, { "LEFT", K_LEFT }, { "RIGHT", K_RIGHT }, { "UP", K_UP },
			{ "A", K_A }, { "B", K_B }, { "C", K_C }, { "X", K_X }, { "Y", K_Y }, { "Z", K_Z },
			{ "L", K_L }, { "R", K_R }, { "SHIFT", K_SHIFT }, { "CTRL", K_CTRL }, { "ALT", K_ALT },
			{ "F5", K_F5 }, { "F6", K_F6 }, { "F7", K_F7 }, { "F8", K_F8 }, { "F9", K_F9 },
		};
		const char *s = rb_id2name(SYM2ID(v));
		for (size_t i = 0; i < sizeof names / sizeof *names; i++)
			if (!strcmp(s, names[i].name))
				return names[i].k;
		return 0;
	}
	if (FIXNUM_P(v)) {
		int k = FIX2INT(v);
		return k > 0 && k < K_MAX ? k : 0;
	}
	return 0;
}

static VALUE i_update(VALUE self) { return Qnil; } /* polled once per frame in Graphics.update */
static VALUE i_press(VALUE self, VALUE k) { return held_frames[key_of(k)] > 0 ? Qtrue : Qfalse; }
static VALUE i_trigger(VALUE self, VALUE k) { return held_frames[key_of(k)] == 1 ? Qtrue : Qfalse; }
static VALUE i_repeat(VALUE self, VALUE k)
{
	/* at once, then after 24 frames every 6 */
	int n = held_frames[key_of(k)];
	return n == 1 || (n > 24 && (n - 24) % 6 == 0) ? Qtrue : Qfalse;
}
static VALUE i_dir4(VALUE self)
{
	static const int order[] = { K_DOWN, K_LEFT, K_RIGHT, K_UP };
	int best = 0, bf = 0;
	for (int i = 0; i < 4; i++)
		if (held_frames[order[i]] > 0 && (!bf || held_frames[order[i]] < bf)) {
			best = order[i];
			bf = held_frames[order[i]];
		}
	return INT2FIX(best);
}
static VALUE i_dir8(VALUE self)
{
	bool d = held_frames[K_DOWN] > 0, l = held_frames[K_LEFT] > 0, r = held_frames[K_RIGHT] > 0, u = held_frames[K_UP] > 0;
	int v = d && !u ? 0 : u && !d ? 6 : 3;     /* row: 1 2 3 / 4 5 6 / 7 8 9 */
	int h = l && !r ? 1 : r && !l ? 3 : 2;
	int dir = v + h;
	return INT2FIX(dir == 5 ? 0 : dir);
}

void init_input(void)
{
	VALUE m = rb_define_module("Input");
	rb_define_module_function(m, "update", i_update, 0);
	rb_define_module_function(m, "press?", i_press, 1);
	rb_define_module_function(m, "trigger?", i_trigger, 1);
	rb_define_module_function(m, "repeat?", i_repeat, 1);
	rb_define_module_function(m, "dir4", i_dir4, 0);
	rb_define_module_function(m, "dir8", i_dir8, 0);
	static const struct { const char *name; int k; } c[] = {
		{ "DOWN", K_DOWN }, { "LEFT", K_LEFT }, { "RIGHT", K_RIGHT }, { "UP", K_UP },
		{ "A", K_A }, { "B", K_B }, { "C", K_C }, { "X", K_X }, { "Y", K_Y }, { "Z", K_Z },
		{ "L", K_L }, { "R", K_R }, { "SHIFT", K_SHIFT }, { "CTRL", K_CTRL }, { "ALT", K_ALT },
		{ "F5", K_F5 }, { "F6", K_F6 }, { "F7", K_F7 }, { "F8", K_F8 }, { "F9", K_F9 },
	};
	/* XP / VX: numbers; VX Ace scripts use symbols (both work) */
	for (size_t i = 0; i < sizeof c / sizeof *c; i++)
		rb_define_const(m, c[i].name, rgss_version >= 3 ? ID2SYM(rb_intern(c[i].name)) : INT2FIX(c[i].k));
}
