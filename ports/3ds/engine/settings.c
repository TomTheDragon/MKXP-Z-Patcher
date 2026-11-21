/* The engine's settings: START opens them on the bottom screen (the game
 * waits meanwhile). Saved in the game's folder (mkxpz-settings.ini). */
#include "engine.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>

Settings settings = {
	.music_volume = 100,
	.sound_volume = 100,
	.buttons = 0,      /* B: confirm, A: cancel, Y: dash */
	.screen = 0,       /* fit */
	.frame_skip = 1,   /* auto */
	.show_speed = 1,
	.overlay = 1,
	.colors = 0,
	.wide_text = 1,
};

#define FILE_NAME "mkxpz-settings.ini"

void settings_load(void)
{
	FILE *f = fopen(FILE_NAME, "r");
	if (!f)
		return;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		int v;
		if (sscanf(line, "music_volume=%d", &v) == 1) settings.music_volume = CLAMP(v, 0, 100);
		else if (sscanf(line, "sound_volume=%d", &v) == 1) settings.sound_volume = CLAMP(v, 0, 100);
		else if (sscanf(line, "buttons=%d", &v) == 1) settings.buttons = CLAMP(v, 0, 1);
		else if (sscanf(line, "screen=%d", &v) == 1) settings.screen = v == 1 ? 1 : 0; /* (the old 1:1 mode: fit) */
		else if (sscanf(line, "frame_skip=%d", &v) == 1) settings.frame_skip = CLAMP(v, 0, 1);
		else if (sscanf(line, "show_speed=%d", &v) == 1) settings.show_speed = CLAMP(v, 0, 1);
		else if (sscanf(line, "overlay=%d", &v) == 1) settings.overlay = CLAMP(v, 0, 1);
		else if (sscanf(line, "colors=%d", &v) == 1) settings.colors = CLAMP(v, 0, 1);
		else if (sscanf(line, "wide_text=%d", &v) == 1) settings.wide_text = CLAMP(v, 0, 1);
	}
	fclose(f);
}

static void settings_save(void)
{
	FILE *f = fopen(FILE_NAME, "w");
	if (!f)
		return;
	fprintf(f, "# MKXP-Z 3DS settings (START in the game)\nmusic_volume=%d\nsound_volume=%d\nbuttons=%d\n"
	        "screen=%d\nwide_text=%d\ncolors=%d\nframe_skip=%d\nshow_speed=%d\noverlay=%d\n", settings.music_volume,
	        settings.sound_volume, settings.buttons, settings.screen, settings.wide_text, settings.colors, settings.frame_skip,
	        settings.show_speed, settings.overlay);
	fclose(f);
}

enum { I_MUSIC, I_SOUND, I_BUTTONS, I_SCREEN, I_TEXT, I_COLORS, I_SKIP, I_OVERLAY, I_SPEED, I_RESET, I_BACK, I_COUNT };

static void show(int sel)
{
	static const char *const buttons[] = { "B confirm, A cancel, Y dash", "A confirm, B cancel, X dash" };
	static const char *const screens[] = { "fit (whole picture)", "stretch (fill)" };
	char v[I_COUNT][40];
	snprintf(v[I_MUSIC], 40, "%d%%", settings.music_volume);
	snprintf(v[I_SOUND], 40, "%d%%", settings.sound_volume);
	snprintf(v[I_BUTTONS], 40, "%s", buttons[settings.buttons]);
	snprintf(v[I_SCREEN], 40, "%s", screens[settings.screen]);
	snprintf(v[I_TEXT], 40, "%s", settings.screen != 0 ? "(only with fit)" :
	         settings.wide_text ? "whole screen width" : "game's width");
	snprintf(v[I_COLORS], 40, "%s", settings.colors ? "vivid (more contrast)" : "normal");
	snprintf(v[I_SKIP], 40, "%s", settings.frame_skip ? "auto (keeps game speed)" : "off");
	snprintf(v[I_OVERLAY], 40, "%s", settings.overlay ? "on (top left)" : "off");
	snprintf(v[I_SPEED], 40, "%s", settings.show_speed ? "on" : "off");
	v[I_RESET][0] = v[I_BACK][0] = 0;
	static const char *const names[I_COUNT] = { "Music volume", "Sound volume", "Buttons", "Screen", "Text boxes", "Colors", "Frame skip",
	                                            "Speed display", "Bottom numbers", "Reset game (title)", "Back to the game" };
	printf("\x1b[2J\x1b[1;1H\x1b[1m MKXP-Z settings\x1b[0m   (START: back)\n\n");
	for (int i = 0; i < I_COUNT; i++) {
		printf("%s %-14s %s\n", i == sel ? "\x1b[33m>" : " ", names[i], v[i]);
		if (i == sel)
			printf("\x1b[0m");
		if (i == I_SPEED)
			printf("\n");
	}
	printf("\n\n Up/Down: choose\n Left/Right: change\n A or B: reset / back\n");
}

/* the menu; returns once it's closed (true: reset the game) */
bool settings_menu(void)
{
	int sel = 0;
	bool reset = false;
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
		if (k & KEY_DOWN) { sel = (sel + 1) % I_COUNT; changed = true; }
		if (k & KEY_UP) { sel = (sel + I_COUNT - 1) % I_COUNT; changed = true; }
		if (dir) {
			changed = true;
			switch (sel) {
			case I_MUSIC: settings.music_volume = CLAMP(settings.music_volume + dir * 10, 0, 100); audio_volumes_changed(); break;
			case I_SOUND: settings.sound_volume = CLAMP(settings.sound_volume + dir * 10, 0, 100); audio_volumes_changed(); break;
			case I_BUTTONS: settings.buttons ^= 1; break;
			case I_SCREEN: settings.screen ^= 1; gpu_set_screen(screen_w, screen_h); break;
			case I_TEXT: settings.wide_text ^= 1; gpu_set_screen(screen_w, screen_h); break;
			case I_COLORS: settings.colors ^= 1; break;
			case I_SKIP: settings.frame_skip ^= 1; break;
			case I_OVERLAY: settings.overlay ^= 1; break;
			case I_SPEED: settings.show_speed ^= 1; break;
			}
		}
		if (k & (KEY_A | KEY_B)) {
			if (sel == I_RESET) {
				reset = true;
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
	/* the buttons held now don't count as presses in the game */
	hidScanInput();
	return reset;
}
