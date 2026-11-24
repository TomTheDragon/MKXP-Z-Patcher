/* The engine's settings: START opens them on the bottom screen (the game
 * waits meanwhile). Saved in the game's folder (mkxpz-settings.ini). */
#include "engine.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>

Settings settings = {
	.music_volume = 100,
	.sound_volume = 100,
	.buttons = 0,      /* A: confirm, B: cancel, Y: run, L: speed-up */
	.screen = 0,       /* fit */
	.frame_skip = 2,   /* auto, at most 2 pictures in a row (0 off, up to 5) */
	.show_speed = 0,
	.log_file = 0,
	.overlay = 1,
	.sharp_text = 1,
	.smooth = 1,
	.battery = 1,
	.activity_icons = 1,
	.wide_screen = 2, /* auto */
	.stereo = 1,
	.colors = 0,
	.wide_text = 1, /* (no longer a setting: always wide, the text in them bigger) */
	.text_size = 150,
	.text_width = 100,
	.same_sound = 4, /* (Changed's shark: many at once, very loud) */
};

#define FILE_NAME "mkxpz-settings.ini"

void settings_load(void)
{
	FILE *f = fopen(FILE_NAME, "r");
	if (!f)
		return;
	char line[128];
	int version = 1, skip = settings.frame_skip, same = settings.same_sound;
	while (fgets(line, sizeof line, f)) {
		int v;
		if (sscanf(line, "version=%d", &v) == 1) version = v;
		else if (sscanf(line, "music_volume=%d", &v) == 1) settings.music_volume = CLAMP(v, 0, 100);
		else if (sscanf(line, "sound_volume=%d", &v) == 1) settings.sound_volume = CLAMP(v, 0, 100);
		else if (sscanf(line, "buttons=%d", &v) == 1) settings.buttons = CLAMP(v, 0, 1);
		else if (sscanf(line, "screen=%d", &v) == 1) settings.screen = CLAMP(v, 0, 2); /* fit, stretch, extended */
		else if (sscanf(line, "frame_skip=%d", &v) == 1) settings.frame_skip = CLAMP(v, 0, 5);
		else if (sscanf(line, "bottom_log=%d", &v) == 1) settings.show_speed = CLAMP(v, 0, 1);
		else if (sscanf(line, "log_file=%d", &v) == 1) settings.log_file = CLAMP(v, 0, 1);
		else if (sscanf(line, "overlay=%d", &v) == 1) settings.overlay = CLAMP(v, 0, 1);
		else if (sscanf(line, "sharp_text=%d", &v) == 1) settings.sharp_text = CLAMP(v, 0, 1);
		else if (sscanf(line, "smooth=%d", &v) == 1) settings.smooth = CLAMP(v, 0, 1);
		else if (sscanf(line, "battery=%d", &v) == 1) settings.battery = CLAMP(v, 0, 1);
		else if (sscanf(line, "activity_icons=%d", &v) == 1) settings.activity_icons = CLAMP(v, 0, 1);
		else if (sscanf(line, "top_screen=%d", &v) == 1) settings.wide_screen = CLAMP(v, 0, 2);
		else if (sscanf(line, "stereo=%d", &v) == 1) settings.stereo = CLAMP(v, 0, 1);
		/* (the older wide_screen=0/1 key: not read, its default was on everywhere) */
		else if (sscanf(line, "colors=%d", &v) == 1) settings.colors = CLAMP(v, 0, 1);
		else if (sscanf(line, "wide_text=%d", &v) == 1) settings.wide_text = CLAMP(v, 0, 1);
		else if (sscanf(line, "text_size=%d", &v) == 1) settings.text_size = CLAMP(v, 100, 200);
		else if (sscanf(line, "text_width=%d", &v) == 1) settings.text_width = CLAMP(v, 100, 200);
		else if (sscanf(line, "same_sound=%d", &v) == 1) settings.same_sound = CLAMP(v, 0, 4);
	}
	fclose(f);
	/* files from before version 2 had other defaults for these two (1 and
	 * 1): they take the new ones once */
	if (version < 2) {
		settings.frame_skip = skip;
		settings.same_sound = same;
	}
	/* (version 2's text_width meant something else: the new defaults) */
	if (version < 3) {
		settings.text_size = 150;
		settings.text_width = 100;
	}
	/* (no longer settings: 3D with the slider, the speed display and sharp
	 * window text are always on) */
	settings.stereo = settings.overlay = settings.sharp_text = 1;
	settings.wide_text = 1; /* (always: wide text boxes with bigger text) */
}

static void settings_save(void)
{
	/* written only when something changed (a write shows the floppy icon) */
	char text[1024];
	int n = snprintf(text, sizeof text, "# MKXP-Z 3DS settings (START in the game)\nversion=3\nmusic_volume=%d\nsound_volume=%d\nbuttons=%d\n"
	        "screen=%d\nwide_text=%d\ncolors=%d\nframe_skip=%d\nbottom_log=%d\nlog_file=%d\noverlay=%d\nsharp_text=%d\nsmooth=%d\nbattery=%d\nactivity_icons=%d\ntop_screen=%d\nstereo=%d\nsame_sound=%d\ntext_size=%d\ntext_width=%d\n", settings.music_volume,
	        settings.sound_volume, settings.buttons, settings.screen, settings.wide_text, settings.colors, settings.frame_skip,
	        settings.show_speed, settings.log_file, settings.overlay, settings.sharp_text, settings.smooth, settings.battery, settings.activity_icons, settings.wide_screen, settings.stereo, settings.same_sound, settings.text_size, settings.text_width);
	if (n <= 0 || n >= (int) sizeof text)
		return;
	char old[1024];
	FILE *f = fopen(FILE_NAME, "r");
	if (f) {
		size_t m = fread(old, 1, sizeof old - 1, f);
		fclose(f);
		old[m] = 0;
		if (!strcmp(old, text))
			return;
	}
	f = fopen(FILE_NAME, "w");
	if (!f)
		return;
	fputs(text, f);
	fclose(f);
}

/* (no longer settings: 3D (always, with the slider), the speed display
 * (always on) and sharp window text (always: no drawback)) */
/* (the actions first: back to the game, reset, the game list (only when
 * started from it)) */
enum { I_BACK, I_RESET, I_LIST, I_MUSIC, I_SOUND, I_SAME, I_BUTTONS, I_SCREEN, I_WIDE, I_TEXTS, I_TEXTW, I_SMOOTH, I_BATTERY, I_ACTIVITY, I_COLORS, I_SKIP, I_SPEED, I_LOG, I_COUNT };

/* (started from the list (MKXP-Z 3DS.3dsx); with one game, which starts at
 * once: "Quit to the Homebrew Launcher") */
static bool item_shown(int i) { return i != I_LIST || from_game_list; }
static int item_step(int sel, int dir)
{
	do
		sel = (sel + dir + I_COUNT) % I_COUNT;
	while (!item_shown(sel));
	return sel;
}

static void show(int sel)
{
	static const char *const buttons[] = { "A:ok B:back L:speed-up", "B:ok A:back L/R:page" };
	static const char *const screens[] = { "fit (whole picture)", "stretch (fill)", "extended (wider map)" };
	char v[I_COUNT][40];
	snprintf(v[I_MUSIC], 40, "%d%%", settings.music_volume);
	snprintf(v[I_SOUND], 40, "%d%%", settings.sound_volume);
	if (settings.same_sound)
		snprintf(v[I_SAME], 40, "max %d at once", settings.same_sound);
	else
		snprintf(v[I_SAME], 40, "no limit");
	snprintf(v[I_BUTTONS], 40, "%s", buttons[settings.buttons]);
	snprintf(v[I_SCREEN], 40, "%s", screens[settings.screen]);
	if (settings.wide_screen == 2)
		snprintf(v[I_WIDE], 40, "auto (%s)", !gpu_wide_possible() ? "400, 2DS" :
		         system_is_emulator() ? "400, emulator" : "800 across");
	else if (settings.wide_screen == 1)
		snprintf(v[I_WIDE], 40, "%s", gpu_wide_possible() ? "on (800 across)" : "on (2DS: no 800)");
	else
		snprintf(v[I_WIDE], 40, "off (400 across)");
	snprintf(v[I_TEXTS], 40, "%d%% (in text boxes)", settings.text_size);
	snprintf(v[I_TEXTW], 40, "%d%% (letters wider)", settings.text_width);
	snprintf(v[I_SMOOTH], 40, "%s", settings.smooth ? "on (smoother edges)" : "off");
	snprintf(v[I_BATTERY], 40, "%s", settings.battery ? "on (top left)" : "off");
	snprintf(v[I_ACTIVITY], 40, "%s", settings.activity_icons ? "on (top right)" : "off");
	snprintf(v[I_COLORS], 40, "%s", settings.colors ? "vivid (more contrast)" : "normal");
	if (settings.frame_skip)
		snprintf(v[I_SKIP], 40, "auto, max %d in a row", settings.frame_skip);
	else
		snprintf(v[I_SKIP], 40, "off");
	snprintf(v[I_SPEED], 40, "%s", settings.show_speed ? "on" : "off");
	snprintf(v[I_LOG], 40, "%s", settings.log_file ? "on (mkxpz-3ds.log)" : "off");
	v[I_RESET][0] = v[I_BACK][0] = v[I_LIST][0] = 0;
	const char *quit = menu_games == 1 ? "Quit to the Homebrew Launcher" : "Quit to the game list";
	static const char *const names[I_COUNT] = { "Back to the game", "Reset game (title)", "", "Music volume", "Sound volume", "Same sound", "Buttons", "Screen", "Top screen", "Text size", "Text width", "Smooth pictures", "Battery icon", "Load/save icons", "Colors", "Frame skip",
	                                            "Bottom log", "Log file" };
	printf("\x1b[2J\x1b[1;1H\x1b[1m MKXP-Z settings\x1b[0m   (START: back)\n\n");
	for (int i = 0; i < I_COUNT; i++) {
		if (!item_shown(i))
			continue;
		printf("%s %-14s %s\n", i == sel ? "\x1b[33m>" : " ", i == I_LIST ? quit : names[i], v[i]);
		if (i == sel)
			printf("\x1b[0m");
		if (i == I_LIST || (i == I_RESET && !item_shown(I_LIST)))
			printf("\n");
	}
	printf("\n Up/Down: choose, Left/Right: change\n A or B: the action (first ones)");
}

/* the menu; returns once it's closed: 0, 1 reset the game, 2 quit to the
 * game list */
int settings_menu(void)
{
	console_force = true; /* (the menu shows also with the bottom log off) */
	int sel = 0;
	int reset = 0;
	hidSetRepeatParameters(20, 6);
	show(sel);
	/* wait until START is released (it opened the menu) */
	while (aptMainLoop()) {
		hidScanInput();
		if (!(hidKeysHeld() & KEY_START))
			break;
		gspWaitForVBlank();
	}
	while (aptMainLoop()) {
		hidScanInput();
		u32 k = hidKeysDown() | (hidKeysDownRepeat() & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN));
		int dir = (k & KEY_RIGHT) ? 1 : (k & KEY_LEFT) ? -1 : 0;
		bool changed = false;
		if (k & KEY_START)
			break;
		if (k & KEY_DOWN) { sel = item_step(sel, 1); changed = true; }
		if (k & KEY_UP) { sel = item_step(sel, -1); changed = true; }
		if (dir) {
			changed = true;
			switch (sel) {
			case I_MUSIC: settings.music_volume = CLAMP(settings.music_volume + dir * 10, 0, 100); audio_volumes_changed(); break;
			case I_SOUND: settings.sound_volume = CLAMP(settings.sound_volume + dir * 10, 0, 100); audio_volumes_changed(); break;
			case I_SAME: settings.same_sound = (settings.same_sound + (dir > 0 ? 1 : 4)) % 5; break;
			case I_BUTTONS: settings.buttons ^= 1; break;
			case I_SCREEN: /* fit, stretch, extended (boot.rb: the wider map at the next frame) */
				settings.screen = (settings.screen + (dir > 0 ? 1 : 2)) % 3;
				gpu_set_screen(screen_w, screen_h);
				break;
			case I_WIDE: /* auto, on, off */
				settings.wide_screen = settings.wide_screen == 2 ? (dir > 0 ? 1 : 0) : settings.wide_screen == 1 ? (dir > 0 ? 0 : 2) : (dir > 0 ? 2 : 1);
				gpu_set_wide();
				break;
			case I_TEXTS: /* 100..200%, the game's size and bigger */
				settings.text_size = settings.text_size + dir * 10 > 200 ? 100 : settings.text_size + dir * 10 < 100 ? 200 : settings.text_size + dir * 10;
				text_layer_epoch++; /* (the boxes' sharp text drawn again) */
				break;
			case I_TEXTW: /* 100..200%: the letters stretched across */
				settings.text_width = settings.text_width + dir * 10 > 200 ? 100 : settings.text_width + dir * 10 < 100 ? 200 : settings.text_width + dir * 10;
				text_layer_epoch++;
				break;
			case I_SMOOTH: settings.smooth ^= 1; break;
			case I_BATTERY: settings.battery ^= 1; break;
			case I_ACTIVITY: settings.activity_icons ^= 1; break;
			case I_COLORS: settings.colors ^= 1; break;
			case I_SKIP: settings.frame_skip = CLAMP(settings.frame_skip + dir, 0, 5); break;
			case I_SPEED: settings.show_speed ^= 1; break;
			case I_LOG: settings.log_file ^= 1; log_set_enabled(settings.log_file); break;
			}
		}
		if (k & (KEY_A | KEY_B)) {
			if (sel == I_RESET || sel == I_LIST) {
				reset = sel == I_RESET ? 1 : 2;
				break;
			}
			if (sel == I_BACK)
				break;
		}
		if (changed)
			show(sel);
		gspWaitForVBlank();
	}
	settings_save();
	printf("\x1b[2J\x1b[1;1H");
	console_force = false;
	/* the buttons held now don't count as presses in the game */
	hidScanInput();
	return reset;
}
