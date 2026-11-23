/* Audio with the 3DS's DSP (ndsp): BGM, BGS and ME are streamed (decoded
 * ahead by an audio thread), sound effects are decoded once and cached.
 * Formats: Ogg Vorbis (Tremor; RPG Maker's LOOPSTART / LOOPLENGTH loop
 * points), WAV (PCM 8/16 bit), MP3 (libmpg123) and MIDI (TinySoundFont
 * with a General MIDI SoundFont: soundfont.sf2 in the game folder or in
 * sdmc:/3ds/mkxpz/; RPG Maker's loop point, controller 111).
 * The DSP needs its firmware, sdmc:/3ds/dspfirm.cdc (made with the "DSP1"
 * homebrew); without it the game runs silently. */
#include "engine.h"
#include <3ds.h>
#include <tremor/ivorbisfile.h>
#define MPG123_NO_LARGENAME
#include <mpg123.h>
#include <math.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- decoders */
enum { SRC_NONE, SRC_OGG, SRC_WAV, SRC_MP3, SRC_MIDI, SRC_ADPCM };

typedef struct {
	int type;
	FILE *f;
	OggVorbis_File vf;
	mpg123_handle *mh;                  /* MP3 */
	tsf *synth;                         /* MIDI: its own copy of the SoundFont's synthesizer */
	tml_message *midi, *msg;            /* MIDI: all messages, the next one */
	unsigned midi_loop_ms;              /* MIDI: controller 111 (RPG Maker's loop point) */
	/* .mkxa (DSP-ADPCM, made by tools/mkxa.c): */
	int16_t coefs[2][16], hist[2][2], loop_hist[2][2];
	int16_t cache[2][14];               /* the decoded frame cache_at */
	long cache_at;                      /* (-1: none) */
	long next_frame;                    /* the next frame in the file */
	uint8_t *abuf;                      /* read buffer */
	int abuf_len, abuf_at;
	int channels, rate;
	long data_start, data_frames, bits; /* WAV */
	long pos;                           /* frames */
	long loop_start, loop_end;          /* frames; loop_end 0: the end */
} Source;

static long comment_num(vorbis_comment *vc, const char *key)
{
	size_t n = strlen(key);
	for (int i = 0; vc && i < vc->comments; i++)
		if (!strncasecmp(vc->user_comments[i], key, n) && vc->user_comments[i][n] == '=')
			return atol(vc->user_comments[i] + n + 1);
	return -1;
}

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* --- MP3 (libmpg123), reading through the FILE (so also from Game.data) */
static mpg123_ssize_t mp3_read(void *h, void *buf, size_t n) { return (mpg123_ssize_t) fread(buf, 1, n, h); }
static off_t mp3_seek(void *h, off_t off, int whence) { return fseek(h, off, whence) ? -1 : ftell(h); }

static bool mp3_open(Source *s)
{
	static bool inited;
	if (!inited) {
		mpg123_init();
		inited = true;
	}
	int err = 0;
	s->mh = mpg123_new(NULL, &err);
	if (!s->mh)
		return false;
	mpg123_param(s->mh, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	/* 16-bit samples at whatever rate the file has */
	mpg123_format_none(s->mh);
	const long *rates;
	size_t nrates;
	mpg123_rates(&rates, &nrates);
	for (size_t i = 0; i < nrates; i++)
		mpg123_format(s->mh, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
	long rate;
	int channels, enc;
	if (mpg123_replace_reader_handle(s->mh, mp3_read, mp3_seek, NULL) != MPG123_OK ||
	    mpg123_open_handle(s->mh, s->f) != MPG123_OK ||
	    mpg123_getformat(s->mh, &rate, &channels, &enc) != MPG123_OK || channels < 1 || channels > 2) {
		mpg123_delete(s->mh);
		s->mh = NULL;
		return false;
	}
	s->type = SRC_MP3;
	s->rate = (int) rate;
	s->channels = channels;
	return true;
}

/* --- .mkxa: DSP-ADPCM (tools/mkxa.c), decoded here for music (cheap);
 * mono sound effects are played by the DSP itself (sound_load) */
#define ADPCM_BUF 4096
static bool adpcm_open(Source *s)
{
	unsigned char h[28];
	if (fseek(s->f, 0, SEEK_SET) || fread(h, 1, 28, s->f) != 28 || memcmp(h, "MKXA", 4) || le32(h + 4) != 1)
		return false;
	s->rate = (int) le32(h + 8);
	s->channels = (int) le32(h + 12);
	s->data_frames = (long) le32(h + 16);
	s->loop_start = (long) le32(h + 20);
	s->loop_end = (long) le32(h + 24);
	if (s->channels < 1 || s->channels > 2 || !s->rate)
		return false;
	for (int c = 0; c < s->channels; c++) {
		unsigned char b[36];
		if (fread(b, 1, 36, s->f) != 36)
			return false;
		for (int k = 0; k < 16; k++)
			s->coefs[c][k] = (int16_t) le16(b + k * 2);
		s->loop_hist[c][0] = (int16_t) le16(b + 32);
		s->loop_hist[c][1] = (int16_t) le16(b + 34);
	}
	s->data_start = 28 + 36 * s->channels;
	s->abuf = malloc(ADPCM_BUF);
	if (!s->abuf)
		return false;
	if (s->loop_start > 0 && !s->loop_end)
		s->loop_end = s->data_frames;
	s->type = SRC_ADPCM;
	s->cache_at = -1;
	return true;
}

static int16_t clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t) v; }

/* the next frame (of each channel) into the cache */
static bool adpcm_next(Source *s)
{
	int need = 8 * s->channels;
	if (s->abuf_at + need > s->abuf_len) {
		int left = s->abuf_len - s->abuf_at;
		memmove(s->abuf, s->abuf + s->abuf_at, left);
		s->abuf_len = left + (int) fread(s->abuf + left, 1, ADPCM_BUF - left, s->f);
		s->abuf_at = 0;
		if (s->abuf_len < need)
			return false;
	}
	for (int c = 0; c < s->channels; c++) {
		const uint8_t *b = s->abuf + s->abuf_at + c * 8;
		int p = b[0] >> 4 & 7, scale = b[0] & 15, c1 = s->coefs[c][p * 2], c2 = s->coefs[c][p * 2 + 1];
		int h1 = s->hist[c][0], h2 = s->hist[c][1];
		for (int i = 0; i < 14; i++) {
			int nib = b[1 + i / 2] >> (i & 1 ? 0 : 4) & 15;
			if (nib >= 8)
				nib -= 16;
			int v = clamp16((((nib << scale) << 11) + 1024 + c1 * h1 + c2 * h2) >> 11);
			h2 = h1;
			h1 = v;
			s->cache[c][i] = (int16_t) v;
		}
		s->hist[c][0] = (int16_t) h1;
		s->hist[c][1] = (int16_t) h2;
	}
	s->abuf_at += need;
	s->cache_at = s->next_frame++;
	return true;
}

static long adpcm_read(Source *s, int16_t *out, long n)
{
	long done = 0;
	while (done < n && s->pos + done < s->data_frames) {
		long at = s->pos + done, f = at / 14;
		while (s->cache_at != f)
			if (!adpcm_next(s))
				return done;
		int i = (int) (at % 14);
		long k = 14 - i;
		if (k > n - done)
			k = n - done;
		if (k > s->data_frames - at)
			k = s->data_frames - at;
		for (long j = 0; j < k; j++)
			for (int c = 0; c < s->channels; c++)
				out[(done + j) * s->channels + c] = s->cache[c][i + j];
		done += k;
	}
	return done;
}

/* to the start or the loop point (the history there is in the file);
 * elsewhere: from the nearer of the two, decoding up to it */
static void adpcm_seek(Source *s, long frame)
{
	long f = frame / 14, from = 0;
	memset(s->hist, 0, sizeof s->hist);
	if (s->loop_start > 0 && f >= s->loop_start / 14) {
		from = s->loop_start / 14;
		memcpy(s->hist, s->loop_hist, sizeof s->hist);
	}
	fseek(s->f, s->data_start + from * 8 * s->channels, SEEK_SET);
	s->abuf_len = s->abuf_at = 0;
	s->next_frame = from;
	s->cache_at = -1;
	while (s->next_frame < f && adpcm_next(s))
		;
}

/* --- MIDI: TinySoundFont, the SoundFont loaded once (the samples stay
 * 16-bit: about the .sf2's size in memory) and shared by every MIDI
 * playing */
#define MIDI_RATE 22050
static tsf *soundfont;
static bool soundfont_tried;
static LightLock sf_lock = 1; /* (LightLock_Init: 1 is unlocked) */

static tsf *soundfont_get(void)
{
	LightLock_Lock(&sf_lock);
	if (!soundfont_tried) {
		soundfont_tried = true;
		static const char *const where[] = { "soundfont.sf2", "../soundfont.sf2", "sdmc:/3ds/mkxpz/soundfont.sf2",
		                                     "sdmc:/3ds/soundfont.sf2", NULL };
		for (int i = 0; where[i] && !soundfont; i++) {
			FILE *f = fopen(where[i], "rb");
			if (!f)
				continue;
			fclose(f);
			soundfont = tsf_load_filename(where[i]);
			log_only("MIDI: SoundFont %s %s\n", where[i], soundfont ? "loaded" : "couldn't be loaded");
		}
		if (!soundfont)
			say("MIDI music needs a SoundFont: soundfont.sf2 in sdmc:/3ds/mkxpz/\n");
	}
	LightLock_Unlock(&sf_lock);
	return soundfont;
}

static bool midi_open(Source *s, const char *path)
{
	tsf *base = soundfont_get();
	if (!base)
		return false;
	s->midi = tml_load_filename(path);
	if (!s->midi)
		return false;
	LightLock_Lock(&sf_lock);
	s->synth = tsf_copy(base);
	LightLock_Unlock(&sf_lock);
	if (!s->synth) {
		tml_free(s->midi);
		s->midi = NULL;
		return false;
	}
	tsf_set_output(s->synth, TSF_STEREO_INTERLEAVED, MIDI_RATE, -3.0f);
	tsf_channel_set_bank_preset(s->synth, 9, 128, 0); /* (channel 10: drums) */
	for (tml_message *m = s->midi; m; m = m->next)
		if (m->type == TML_CONTROL_CHANGE && m->control == 111) {
			s->midi_loop_ms = m->time;
			break;
		}
	s->msg = s->midi;
	s->type = SRC_MIDI;
	s->rate = MIDI_RATE;
	s->channels = 2;
	if (s->midi_loop_ms)
		s->loop_start = (long) ((long long) s->midi_loop_ms * MIDI_RATE / 1000);
	return true;
}

/* the messages up to ms */
static void midi_feed(Source *s, unsigned ms)
{
	for (; s->msg && s->msg->time <= ms; s->msg = s->msg->next) {
		tml_message *m = s->msg;
		switch (m->type) {
		case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(s->synth, m->channel, m->program, m->channel == 9); break;
		case TML_NOTE_ON: tsf_channel_note_on(s->synth, m->channel, m->key, m->velocity / 127.0f); break;
		case TML_NOTE_OFF: tsf_channel_note_off(s->synth, m->channel, m->key); break;
		case TML_PITCH_BEND: tsf_channel_set_pitchwheel(s->synth, m->channel, m->pitch_bend); break;
		case TML_CONTROL_CHANGE: tsf_channel_midi_control(s->synth, m->channel, m->control, m->control_value); break;
		}
	}
}

static long midi_read(Source *s, int16_t *out, long n)
{
	long done = 0;
	while (done < n) {
		if (!s->msg && !tsf_active_voice_count(s->synth))
			break; /* the end */
		long block = n - done < 64 ? n - done : 64;
		midi_feed(s, (unsigned) ((long long) (s->pos + done) * 1000 / MIDI_RATE));
		tsf_render_short(s->synth, out + done * 2, (int) block, 0);
		done += block;
	}
	return done;
}

/* (only to the start or the loop point: messages from there; the notes
 * sounding stop) */
static void midi_seek(Source *s, long frame)
{
	unsigned ms = (unsigned) ((long long) frame * 1000 / MIDI_RATE);
	tsf_note_off_all(s->synth);
	if (ms == 0) {
		tsf_reset(s->synth);
		tsf_channel_set_bank_preset(s->synth, 9, 128, 0);
		s->msg = s->midi;
		return;
	}
	/* (programs, controllers from the start up to there: notes left out) */
	for (s->msg = s->midi; s->msg && s->msg->time < ms; s->msg = s->msg->next) {
		tml_message *m = s->msg;
		if (m->type == TML_PROGRAM_CHANGE)
			tsf_channel_set_presetnumber(s->synth, m->channel, m->program, m->channel == 9);
		else if (m->type == TML_CONTROL_CHANGE)
			tsf_channel_midi_control(s->synth, m->channel, m->control, m->control_value);
		else if (m->type == TML_PITCH_BEND)
			tsf_channel_set_pitchwheel(s->synth, m->channel, m->pitch_bend);
	}
}

static bool src_open(Source *s, const char *path)
{
	memset(s, 0, sizeof *s);
	{
		size_t l = strlen(path);
		if ((l > 4 && !strcasecmp(path + l - 4, ".mid")) || (l > 5 && !strcasecmp(path + l - 5, ".midi")))
			return midi_open(s, path);
	}
	s->f = fopen(path, "rb");
	if (!s->f)
		return false;
	unsigned char head[12];
	if (fread(head, 1, 12, s->f) == 12 && !memcmp(head, "MKXA", 4)) {
		if (adpcm_open(s))
			return true;
		free(s->abuf);
		s->abuf = NULL;
		fclose(s->f);
		s->f = NULL;
		return false;
	}
	if (!memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) {
		/* WAV: find "fmt " and "data" */
		unsigned char ch[8];
		while (fread(ch, 1, 8, s->f) == 8) {
			uint32_t len = le32(ch + 4);
			if (!memcmp(ch, "fmt ", 4)) {
				unsigned char fmt[16];
				if (len < 16 || fread(fmt, 1, 16, s->f) != 16)
					break;
				if (le16(fmt) != 1) /* PCM only */
					break;
				s->channels = le16(fmt + 2);
				s->rate = le32(fmt + 4);
				s->bits = le16(fmt + 14);
				fseek(s->f, len - 16 + (len & 1), SEEK_CUR);
			} else if (!memcmp(ch, "data", 4)) {
				if (!s->channels || (s->bits != 8 && s->bits != 16) || s->channels > 2)
					break;
				s->data_start = ftell(s->f);
				s->data_frames = len / (s->channels * s->bits / 8);
				s->type = SRC_WAV;
				return true;
			} else {
				fseek(s->f, len + (len & 1), SEEK_CUR);
			}
		}
		fclose(s->f);
		s->f = NULL;
		return false;
	}
	/* MP3: an ID3 tag or an MPEG frame header first (or the name says so) */
	size_t pl = strlen(path);
	if ((pl > 4 && !strcasecmp(path + pl - 4, ".mp3")) || !memcmp(head, "ID3", 3) ||
	    (head[0] == 0xff && (head[1] & 0xe0) == 0xe0)) {
		rewind(s->f);
		if (mp3_open(s))
			return true;
		fclose(s->f);
		s->f = NULL;
		return false;
	}
	rewind(s->f);
	if (ov_open(s->f, &s->vf, NULL, 0) != 0) {
		fclose(s->f);
		s->f = NULL;
		return false;
	}
	vorbis_info *vi = ov_info(&s->vf, -1);
	if (!vi || vi->channels < 1 || vi->channels > 2) {
		ov_clear(&s->vf); /* (closes the file) */
		s->f = NULL;
		return false;
	}
	s->type = SRC_OGG;
	s->channels = vi->channels;
	s->rate = vi->rate;
	vorbis_comment *vc = ov_comment(&s->vf, -1);
	long ls = comment_num(vc, "LOOPSTART"), ll = comment_num(vc, "LOOPLENGTH");
	if (ls >= 0 && ll > 0) {
		s->loop_start = ls;
		s->loop_end = ls + ll;
	}
	return true;
}

static void src_close(Source *s)
{
	if (s->type == SRC_MIDI) {
		tsf_close(s->synth);
		tml_free(s->midi);
		s->synth = NULL;
		s->midi = s->msg = NULL;
	} else if (s->type == SRC_OGG) {
		ov_clear(&s->vf);
	} else {
		if (s->mh) {
			mpg123_close(s->mh);
			mpg123_delete(s->mh);
			s->mh = NULL;
		}
		if (s->f)
			fclose(s->f);
		free(s->abuf);
		s->abuf = NULL;
	}
	s->type = SRC_NONE;
	s->f = NULL;
}

static void src_seek(Source *s, long frame)
{
	if (frame < 0)
		frame = 0;
	if (s->type == SRC_OGG)
		ov_pcm_seek(&s->vf, frame);
	else if (s->type == SRC_WAV)
		fseek(s->f, s->data_start + frame * s->channels * (s->bits / 8), SEEK_SET);
	else if (s->type == SRC_MP3)
		mpg123_seek(s->mh, frame, SEEK_SET);
	else if (s->type == SRC_MIDI)
		midi_seek(s, frame);
	else if (s->type == SRC_ADPCM)
		adpcm_seek(s, frame);
	s->pos = frame;
}

/* up to n frames of 16-bit samples; at the loop end it goes back to the loop
 * start (loop) or stops; returns frames read (0: the end) */
static long src_read(Source *s, int16_t *out, long n, bool loop)
{
	long done = 0;
	while (done < n) {
		long want = n - done;
		if (s->loop_end > 0 && loop && s->pos + want > s->loop_end)
			want = s->loop_end - s->pos;
		long got = 0;
		if (want > 0) {
			if (s->type == SRC_OGG) {
				int bs;
				long bytes = ov_read(&s->vf, (char *) (out + done * s->channels), want * s->channels * 2, &bs);
				got = bytes > 0 ? bytes / (2 * s->channels) : 0;
			} else if (s->type == SRC_ADPCM) {
				got = adpcm_read(s, out + done * s->channels, want);
			} else if (s->type == SRC_MIDI) {
				got = midi_read(s, out + done * s->channels, want);
			} else if (s->type == SRC_MP3) {
				size_t bytes = 0;
				int rc = mpg123_read(s->mh, (unsigned char *) (out + done * s->channels), want * s->channels * 2, &bytes);
				got = (rc == MPG123_OK || rc == MPG123_DONE || bytes) ? (long) (bytes / (2 * s->channels)) : 0;
			} else if (s->type == SRC_WAV) {
				long left = s->data_frames - s->pos;
				if (want > left)
					want = left;
				if (s->bits == 16) {
					got = fread(out + done * s->channels, 2 * s->channels, want, s->f);
				} else {
					uint8_t tmp[1024];
					long chunk = want * s->channels < 1024 ? want : 1024 / s->channels;
					got = fread(tmp, s->channels, chunk, s->f);
					for (long i = 0; i < got * s->channels; i++)
						out[done * s->channels + i] = (int16_t) ((tmp[i] - 128) << 8);
				}
			}
		}
		if (got <= 0) {
			if (!loop || (done == 0 && (s->loop_end > 0 ? s->pos == s->loop_start : s->pos == 0)))
				break; /* the end (or nothing to loop) */
			src_seek(s, s->loop_end > 0 ? s->loop_start : 0);
			continue;
		}
		done += got;
		s->pos += got;
	}
	return done;
}

/* --- channels: 0 BGM, 1 BGS, 2 ME, 3.. sound effects */
#define CH_BGM 0
#define CH_BGS 1
#define CH_ME 2
#define CH_SE0 3
#define CH_SE_N 16
#define BUF_FRAMES 4096
#define NBUF 4

typedef struct {
	int ch;
	LightLock lock;
	/* requests from Ruby (done by the audio thread) */
	bool req_play, req_stop;
	char req_name[256];
	int req_volume, req_pitch;
	long req_pos;
	int req_fade_ms;
	/* state */
	Source src;
	bool playing, ended, loop, paused;
	char name[256];
	int volume, pitch;
	float fade, fade_step; /* volume factor while fading out */
	int16_t *bufs[NBUF];
	ndspWaveBuf wb[NBUF];
	long buf_pos[NBUF];    /* source frame at each buffer's start (for bgm_pos) */
} Stream;

static Stream streams[3];

static bool audio_ok;
static Thread thread;
static volatile bool quit;
static LightEvent wake;

static float vol_factor(int v) { v = CLAMP(v, 0, 100); return v / 100.0f; }
/* with the settings' music (BGM, BGS, ME) or sound (SE) volume */
static float music_factor(int v) { return vol_factor(v) * settings.music_volume / 100.0f; }
static float sound_factor(int v) { return vol_factor(v) * settings.sound_volume / 100.0f; }

static void set_mix(int ch, float v)
{
	float mix[12] = { 0 };
	mix[0] = mix[1] = v;
	ndspChnSetMix(ch, mix);
}

/* find the file: with or without extension */
static bool find_audio(const char *name, char *out, size_t n)
{
	static const char *const ext[] = { ".mkxa", ".ogg", ".wav", ".oga", ".mp3", ".mid", ".midi", "", NULL };
	return file_find(name, ext, out, n);
}

static void stream_stop(Stream *s)
{
	ndspChnWaveBufClear(s->ch);
	src_close(&s->src);
	s->playing = false;
	s->name[0] = 0;
	if (s->ch == CH_ME && streams[CH_BGM].paused) {
		/* the ME is over: the BGM goes on */
		ndspChnSetPaused(CH_BGM, false);
		streams[CH_BGM].paused = false;
	}
}

static void stream_start(Stream *s)
{
	char path[300];
	stream_stop(s);
	if (!find_audio(s->req_name, path, sizeof path) || !src_open(&s->src, path))
		return; /* missing or unsupported (e.g. MIDI): silent */
	snprintf(s->name, sizeof s->name, "%s", s->req_name);
	s->volume = s->req_volume;
	s->pitch = s->req_pitch;
	s->loop = s->ch != CH_ME;
	s->fade = 1;
	s->fade_step = 0;
	s->req_fade_ms = 0;
	s->ended = false;
	if (s->req_pos > 0)
		src_seek(&s->src, s->req_pos);
	ndspChnReset(s->ch);
	ndspChnSetInterp(s->ch, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(s->ch, s->src.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetRate(s->ch, s->src.rate * CLAMP(s->pitch, 50, 150) / 100.0f);
	set_mix(s->ch, music_factor(s->volume));
	for (int i = 0; i < NBUF; i++)
		s->wb[i].status = NDSP_WBUF_DONE; /* to be filled */
	s->playing = true;
	if (s->ch == CH_ME && streams[CH_BGM].playing) {
		/* the BGM waits while an ME plays */
		ndspChnSetPaused(CH_BGM, true);
		streams[CH_BGM].paused = true;
	}
}

/* fill finished buffers */
static void stream_feed(Stream *s)
{
	if (!s->playing)
		return;
	bool any_queued = false;
	for (int i = 0; i < NBUF; i++) {
		ndspWaveBuf *w = &s->wb[i];
		if (w->status != NDSP_WBUF_DONE && w->status != NDSP_WBUF_FREE) {
			any_queued = true;
			continue;
		}
		if (s->ended)
			continue;
		s->buf_pos[i] = s->src.pos;
		long got = src_read(&s->src, s->bufs[i], BUF_FRAMES, s->loop);
		if (got <= 0) {
			s->ended = true;
			continue;
		}
		memset(w, 0, sizeof *w);
		w->data_pcm16 = s->bufs[i];
		w->nsamples = got;
		DSP_FlushDataCache(s->bufs[i], got * s->src.channels * 2);
		ndspChnWaveBufAdd(s->ch, w);
		any_queued = true;
	}
	if (s->ended && !any_queued)
		stream_stop(s); /* played to the end (ME, or a BGM without loop) */
}

static void audio_thread(void *arg)
{
	while (!quit) {
		LightEvent_WaitTimeout(&wake, 10 * 1000000LL);
		for (int i = 0; i < 3; i++) {
			Stream *s = &streams[i];
			LightLock_Lock(&s->lock);
			if (s->req_stop) {
				s->req_stop = false;
				stream_stop(s);
			}
			if (s->req_play) {
				s->req_play = false;
				stream_start(s);
			}
			if (s->req_fade_ms > 0) {
				/* down to silence in that time (ticks of about 10 ms); only
				 * for what plays now (not the next music) */
				if (s->playing)
					s->fade_step = 10.0f / s->req_fade_ms;
				s->req_fade_ms = 0;
			}
			if (s->playing && s->fade_step > 0) {
				s->fade -= s->fade_step;
				if (s->fade <= 0)
					stream_stop(s);
				else
					set_mix(s->ch, music_factor(s->volume) * s->fade);
			}
			stream_feed(s);
			LightLock_Unlock(&s->lock);
		}
	}
}

/* --- sound effects: decoded once, cached */
typedef struct {
	char name[128];
	int16_t *pcm; /* linear memory (PCM16, or the ADPCM frames) */
	long frames;
	int channels, rate;
	unsigned used; /* for evicting the least recently used */
	size_t bytes;  /* of pcm */
	bool adpcm;    /* mono .mkxa: played by the DSP as it is */
	u16 coefs[16];
	ndspAdpcmData adata;
} Sound;

#define SE_CACHE 48
/* (decoded sound effects live in linear memory, which textures need too:
 * at most this much; Pocket Mirror's 55 effects took almost all of it) */
#define SE_BYTES (4u * 1024 * 1024)
static Sound sounds[SE_CACHE];
static size_t se_bytes;
static unsigned use_clock;
static ndspWaveBuf se_wb[CH_SE_N];
static int next_se;

/* lets go of the least recently used sound effect that isn't playing;
 * false if there's none (any: also from textures, gpu.c, when graphics
 * memory runs short) */
static bool se_release_one(bool any)
{
	(void) any;
	Sound *old = NULL;
	for (int i = 0; i < SE_CACHE; i++) {
		if (!sounds[i].pcm)
			continue;
		bool playing = false;
		for (int c = 0; c < CH_SE_N; c++)
			if (se_wb[c].data_pcm16 == sounds[i].pcm && ndspChnIsPlaying(CH_SE0 + c))
				playing = true;
		if (!playing && (!old || sounds[i].used < old->used))
			old = &sounds[i];
	}
	if (!old)
		return false;
	for (int c = 0; c < CH_SE_N; c++)
		if (se_wb[c].data_pcm16 == old->pcm) {
			ndspChnWaveBufClear(CH_SE0 + c);
			se_wb[c].data_pcm16 = NULL;
		}
	linearFree(old->pcm);
	se_bytes -= old->bytes;
	old->pcm = NULL;
	return true;
}

bool audio_release_memory(void)
{
	return audio_ok && se_release_one(true);
}

static Sound *sound_load(const char *name)
{
	for (int i = 0; i < SE_CACHE; i++)
		if (sounds[i].pcm && !strcmp(sounds[i].name, name)) {
			sounds[i].used = ++use_clock;
			return &sounds[i];
		}
	char path[300];
	Source src;
	u64 t_load = svcGetSystemTick();
	if (!find_audio(name, path, sizeof path) || !src_open(&src, path))
		return NULL;
	if (src.type == SRC_ADPCM && src.channels == 1) {
		/* mono DSP-ADPCM: kept as it is (a quarter of PCM16), the DSP
		 * decodes it */
		size_t bytes = (size_t) (src.data_frames + 13) / 14 * 8;
		Sound *slot = NULL;
		for (int i = 0; i < SE_CACHE && !slot; i++)
			if (!sounds[i].pcm)
				slot = &sounds[i];
		while ((!slot || se_bytes + bytes > SE_BYTES) && se_release_one(false))
			for (int i = 0; i < SE_CACHE && !slot; i++)
				if (!sounds[i].pcm)
					slot = &sounds[i];
		uint8_t *data = slot ? linearAlloc(bytes) : NULL;
		if (data && (fseek(src.f, src.data_start, SEEK_SET) || fread(data, 1, bytes, src.f) != bytes)) {
			linearFree(data);
			data = NULL;
		}
		if (data) {
			DSP_FlushDataCache(data, bytes);
			snprintf(slot->name, sizeof slot->name, "%s", name);
			slot->pcm = (int16_t *) data;
			slot->frames = src.data_frames;
			slot->channels = 1;
			slot->rate = src.rate;
			slot->used = ++use_clock;
			slot->bytes = bytes;
			slot->adpcm = true;
			for (int k = 0; k < 16; k++)
				slot->coefs[k] = (u16) src.coefs[0][k];
			se_bytes += bytes;
		}
		log_slow_load("sound", name, t_load);
		src_close(&src);
		return data ? slot : NULL;
	}
	/* decode it all (sound effects are short) */
	long cap = 44100, n = 0;
	int16_t *tmp = malloc(cap * src.channels * 2);
	while (tmp) {
		loading_tick(-1);
		if (n == cap) {
			int16_t *g = realloc(tmp, cap * 2 * src.channels * 2);
			if (!g)
				break;
			tmp = g;
			cap *= 2;
		}
		long got = src_read(&src, tmp + n * src.channels, cap - n, false);
		if (got <= 0)
			break;
		n += got;
	}
	Sound *slot = &sounds[0];
	for (int i = 0; i < SE_CACHE; i++) {
		if (!sounds[i].pcm) {
			slot = &sounds[i];
			break;
		}
		if (sounds[i].used < slot->used)
			slot = &sounds[i];
	}
	if (slot->pcm) {
		/* still playing somewhere? stop those channels first */
		for (int c = 0; c < CH_SE_N; c++)
			if (se_wb[c].data_pcm16 == slot->pcm) {
				ndspChnWaveBufClear(CH_SE0 + c);
				se_wb[c].data_pcm16 = NULL;
			}
		linearFree(slot->pcm);
		se_bytes -= slot->bytes;
		slot->pcm = NULL;
	}
	/* within the budget: the least recently used ones go */
	while (n > 0 && se_bytes + (size_t) n * src.channels * 2 > SE_BYTES && se_release_one(false))
		;
	int16_t *pcm = n > 0 ? linearAlloc(n * src.channels * 2) : NULL;
	if (pcm) {
		memcpy(pcm, tmp, n * src.channels * 2);
		DSP_FlushDataCache(pcm, n * src.channels * 2);
		snprintf(slot->name, sizeof slot->name, "%s", name);
		slot->pcm = pcm;
		slot->frames = n;
		slot->channels = src.channels;
		slot->rate = src.rate;
		slot->used = ++use_clock;
		slot->bytes = (size_t) n * src.channels * 2;
		slot->adpcm = false;
		se_bytes += slot->bytes;
	}
	log_slow_load("sound", name, t_load);
	free(tmp);
	src_close(&src);
	return pcm ? slot : NULL;
}

/* --- Ruby: Audio */
static void request_play(int i, int argc, VALUE *argv)
{
	if (!audio_ok || argc < 1)
		return;
	Stream *s = &streams[i];
	const char *name = StringValueCStr(argv[0]);
	int vol = argc > 1 ? num2int(argv[1]) : (i == CH_ME || i == CH_BGM ? 100 : 80);
	int pitch = argc > 2 ? num2int(argv[2]) : 100;
	long pos = argc > 3 ? NUM2LONG(argv[3]) : 0;
	LightLock_Lock(&s->lock);
	if (s->playing && !strcmp(s->name, name) && s->pitch == pitch && pos == 0 && s->fade_step == 0) {
		/* the same music: it goes on, maybe at another volume */
		s->volume = vol;
		set_mix(s->ch, music_factor(vol));
	} else {
		snprintf(s->req_name, sizeof s->req_name, "%s", name);
		s->req_volume = vol;
		s->req_pitch = pitch;
		s->req_pos = pos;
		s->req_play = true;
		s->req_stop = false;
	}
	LightLock_Unlock(&s->lock);
	LightEvent_Signal(&wake);
}

static void request_stop(int i)
{
	if (!audio_ok)
		return;
	Stream *s = &streams[i];
	LightLock_Lock(&s->lock);
	s->req_play = false;
	s->req_stop = true;
	LightLock_Unlock(&s->lock);
	LightEvent_Signal(&wake);
}

static void request_fade(int i, VALUE ms)
{
	if (!audio_ok)
		return;
	Stream *s = &streams[i];
	LightLock_Lock(&s->lock);
	if (s->playing || s->req_play)
		s->req_fade_ms = num2int(ms) > 0 ? num2int(ms) : 1;
	LightLock_Unlock(&s->lock);
	LightEvent_Signal(&wake);
}

static VALUE stream_pos(int i)
{
	if (!audio_ok)
		return INT2FIX(0);
	Stream *s = &streams[i];
	LightLock_Lock(&s->lock);
	long pos = 0;
	if (s->playing) {
		/* the oldest queued buffer is the one playing */
		u16 seq = 0xffff;
		int cur = -1;
		for (int b = 0; b < NBUF; b++)
			if (s->wb[b].status == NDSP_WBUF_PLAYING || s->wb[b].status == NDSP_WBUF_QUEUED) {
				if (cur < 0 || (u16) (s->wb[b].sequence_id - seq) > 0x8000) {
					cur = b;
					seq = s->wb[b].sequence_id;
				}
			}
		pos = cur >= 0 ? s->buf_pos[cur] + (long) ndspChnGetSamplePos(s->ch) : s->src.pos;
	}
	LightLock_Unlock(&s->lock);
	return LONG2NUM(pos);
}

static VALUE a_bgm_play(int argc, VALUE *argv, VALUE self) { request_play(CH_BGM, argc, argv); return Qnil; }
static VALUE a_bgm_stop(VALUE self) { request_stop(CH_BGM); return Qnil; }
static VALUE a_bgm_fade(VALUE self, VALUE ms) { request_fade(CH_BGM, ms); return Qnil; }
static VALUE a_bgm_pos(VALUE self) { return stream_pos(CH_BGM); }
static VALUE a_bgs_play(int argc, VALUE *argv, VALUE self) { request_play(CH_BGS, argc, argv); return Qnil; }
static VALUE a_bgs_stop(VALUE self) { request_stop(CH_BGS); return Qnil; }
static VALUE a_bgs_fade(VALUE self, VALUE ms) { request_fade(CH_BGS, ms); return Qnil; }
static VALUE a_bgs_pos(VALUE self) { return stream_pos(CH_BGS); }
static VALUE a_me_play(int argc, VALUE *argv, VALUE self) { request_play(CH_ME, argc, argv); return Qnil; }
static VALUE a_me_stop(VALUE self) { request_stop(CH_ME); return Qnil; }
static VALUE a_me_fade(VALUE self, VALUE ms) { request_fade(CH_ME, ms); return Qnil; }

/* (boot.rb: a map's sound effects decoded ahead, into the cache; only
 * while 3 MB of linear memory are left) */
static VALUE a_preload_se(VALUE self, VALUE name)
{
	if (audio_ok && linearSpaceFree() >= 2u * 1024 * 1024 && se_bytes < 3u * 1024 * 1024)
		sound_load(StringValueCStr(name));
	return Qnil;
}

static VALUE a_se_play(int argc, VALUE *argv, VALUE self)
{
	if (!audio_ok || argc < 1)
		return Qnil;
	Sound *snd = sound_load(StringValueCStr(argv[0]));
	if (!snd)
		return Qnil;
	int vol = argc > 1 ? num2int(argv[1]) : 80, pitch = argc > 2 ? num2int(argv[2]) : 100;
	/* a free channel, else the oldest one */
	int c = -1;
	for (int i = 0; i < CH_SE_N && c < 0; i++)
		if (!ndspChnIsPlaying(CH_SE0 + (next_se + i) % CH_SE_N))
			c = (next_se + i) % CH_SE_N;
	if (c < 0)
		c = next_se;
	next_se = (c + 1) % CH_SE_N;
	int ch = CH_SE0 + c;
	ndspChnWaveBufClear(ch);
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(ch, snd->adpcm ? NDSP_FORMAT_MONO_ADPCM
	                                : snd->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetRate(ch, snd->rate * CLAMP(pitch, 50, 150) / 100.0f);
	set_mix(ch, sound_factor(vol));
	memset(&se_wb[c], 0, sizeof se_wb[c]);
	se_wb[c].data_pcm16 = snd->pcm;
	se_wb[c].nsamples = snd->frames;
	if (snd->adpcm) {
		ndspChnSetAdpcmCoefs(ch, snd->coefs);
		/* (the decoder's start: the first frame's header, no history) */
		snd->adata.index = ((const uint8_t *) snd->pcm)[0];
		snd->adata.history0 = snd->adata.history1 = 0;
		se_wb[c].adpcm_data = &snd->adata;
	}
	ndspChnWaveBufAdd(ch, &se_wb[c]);
	return Qnil;
}

static VALUE a_se_stop(VALUE self)
{
	if (audio_ok)
		for (int c = 0; c < CH_SE_N; c++)
			ndspChnWaveBufClear(CH_SE0 + c);
	return Qnil;
}

static VALUE a_nothing(int argc, VALUE *argv, VALUE self) { return Qnil; }

/* the settings' volumes changed: the music at once (sound effects: the next ones) */
void audio_volumes_changed(void)
{
	if (!audio_ok)
		return;
	for (int i = 0; i < 3; i++) {
		Stream *s = &streams[i];
		LightLock_Lock(&s->lock);
		if (s->playing)
			set_mix(s->ch, music_factor(s->volume) * s->fade);
		LightLock_Unlock(&s->lock);
	}
}

bool audio_start(void)
{
	if (R_FAILED(ndspInit()))
		return false;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	LightEvent_Init(&wake, RESET_ONESHOT);
	for (int i = 0; i < 3; i++) {
		Stream *s = &streams[i];
		s->ch = i;
		LightLock_Init(&s->lock);
		for (int b = 0; b < NBUF; b++) {
			s->bufs[b] = linearAlloc(BUF_FRAMES * 2 * 2);
			s->wb[b].status = NDSP_WBUF_FREE;
		}
	}
	/* above the main thread (0x30), so the music doesn't stutter; on another
	 * core than the game: the New 3DS's core 2, else a share of the system
	 * core (core 1), else the game's own core */
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	thread = NULL;
	if (n3ds)
		thread = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, 2, false);
	if (!thread && R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)))
		thread = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, 1, false);
	if (!thread)
		thread = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, -2, false);
	audio_ok = thread != NULL;
	return audio_ok;
}

void audio_stop(void)
{
	if (!audio_ok)
		return;
	quit = true;
	LightEvent_Signal(&wake);
	threadJoin(thread, U64_MAX);
	threadFree(thread);
	ndspExit();
	audio_ok = false;
}

void init_audio(void)
{
	VALUE m = rb_define_module("Audio");
	rb_define_module_function(m, "bgm_play", a_bgm_play, -1);
	rb_define_module_function(m, "bgm_stop", a_bgm_stop, 0);
	rb_define_module_function(m, "bgm_fade", a_bgm_fade, 1);
	rb_define_module_function(m, "bgm_pos", a_bgm_pos, 0);
	rb_define_module_function(m, "bgs_play", a_bgs_play, -1);
	rb_define_module_function(m, "bgs_stop", a_bgs_stop, 0);
	rb_define_module_function(m, "bgs_fade", a_bgs_fade, 1);
	rb_define_module_function(m, "bgs_pos", a_bgs_pos, 0);
	rb_define_module_function(m, "me_play", a_me_play, -1);
	rb_define_module_function(m, "me_stop", a_me_stop, 0);
	rb_define_module_function(m, "me_fade", a_me_fade, 1);
	rb_define_module_function(m, "se_play", a_se_play, -1);
	rb_define_module_function(m, "mkxpz_preload_se", a_preload_se, 1);
	rb_define_module_function(m, "se_stop", a_se_stop, 0);
	rb_define_module_function(m, "setup_midi", a_nothing, -1);
}

/* --- a movie's sound (movie.c): its own DSP channel, a ring of buffers;
 * the samples played so far are the movie's clock */
#define CH_MOVIE (CH_SE0 + CH_SE_N)
#define MOVIE_BUFS 8
#define MOVIE_FRAMES 2048
static ndspWaveBuf movie_wb[MOVIE_BUFS];
static int16_t *movie_pcm;
static int movie_channels, movie_next;
static long long movie_queued; /* frames given to the DSP */

bool audio_movie_open(int rate, int channels)
{
	if (!audio_ok || channels < 1 || channels > 2)
		return false;
	movie_pcm = linearAlloc(MOVIE_BUFS * MOVIE_FRAMES * channels * 2);
	if (!movie_pcm)
		return false;
	movie_channels = channels;
	movie_next = 0;
	movie_queued = 0;
	memset(movie_wb, 0, sizeof movie_wb);
	for (int i = 0; i < MOVIE_BUFS; i++)
		movie_wb[i].status = NDSP_WBUF_DONE;
	ndspChnReset(CH_MOVIE);
	ndspChnSetInterp(CH_MOVIE, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(CH_MOVIE, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetRate(CH_MOVIE, (float) rate);
	set_mix(CH_MOVIE, music_factor(100));
	return true;
}

/* a free buffer to fill (MOVIE_FRAMES frames), or NULL */
int16_t *audio_movie_buffer(void)
{
	if (!movie_pcm)
		return NULL;
	ndspWaveBuf *wb = &movie_wb[movie_next];
	if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
		return NULL;
	return movie_pcm + (size_t) movie_next * MOVIE_FRAMES * movie_channels;
}

int audio_movie_buffer_frames(void) { return MOVIE_FRAMES; }

/* the buffer from audio_movie_buffer, with frames filled: played */
void audio_movie_queue(int frames)
{
	ndspWaveBuf *wb = &movie_wb[movie_next];
	int16_t *p = movie_pcm + (size_t) movie_next * MOVIE_FRAMES * movie_channels;
	memset(wb, 0, sizeof *wb);
	wb->data_pcm16 = p;
	wb->nsamples = frames;
	DSP_FlushDataCache(p, frames * movie_channels * 2);
	ndspChnWaveBufAdd(CH_MOVIE, wb);
	movie_queued += frames;
	movie_next = (movie_next + 1) % MOVIE_BUFS;
}

/* frames played so far (the clock): the queued ones minus what's still
 * waiting */
long long audio_movie_played(void)
{
	if (!movie_pcm)
		return 0;
	long long waiting = 0;
	bool playing_seen = false;
	for (int i = 0; i < MOVIE_BUFS; i++) {
		if (movie_wb[i].status == NDSP_WBUF_QUEUED)
			waiting += movie_wb[i].nsamples;
		else if (movie_wb[i].status == NDSP_WBUF_PLAYING) {
			waiting += movie_wb[i].nsamples - ndspChnGetSamplePos(CH_MOVIE);
			playing_seen = true;
		}
	}
	(void) playing_seen;
	return movie_queued - waiting;
}

bool audio_movie_idle(void)
{
	for (int i = 0; i < MOVIE_BUFS; i++)
		if (movie_wb[i].status == NDSP_WBUF_QUEUED || movie_wb[i].status == NDSP_WBUF_PLAYING)
			return false;
	return true;
}

void audio_movie_close(void)
{
	if (!movie_pcm)
		return;
	ndspChnWaveBufClear(CH_MOVIE);
	ndspChnReset(CH_MOVIE);
	linearFree(movie_pcm);
	movie_pcm = NULL;
}
