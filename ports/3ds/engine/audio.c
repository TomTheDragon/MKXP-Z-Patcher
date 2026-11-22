/* Audio with the 3DS's DSP (ndsp): BGM, BGS and ME are streamed (decoded
 * ahead by an audio thread), sound effects are decoded once and cached.
 * Formats: Ogg Vorbis (Tremor; RPG Maker's LOOPSTART / LOOPLENGTH loop
 * points) and WAV (PCM 8/16 bit). MIDI: not yet (silent).
 * The DSP needs its firmware, sdmc:/3ds/dspfirm.cdc (made with the "DSP1"
 * homebrew); without it the game runs silently. */
#include "engine.h"
#include <3ds.h>
#include <tremor/ivorbisfile.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- decoders */
enum { SRC_NONE, SRC_OGG, SRC_WAV };

typedef struct {
	int type;
	FILE *f;
	OggVorbis_File vf;
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

static bool src_open(Source *s, const char *path)
{
	memset(s, 0, sizeof *s);
	s->f = fopen(path, "rb");
	if (!s->f)
		return false;
	unsigned char head[12];
	if (fread(head, 1, 12, s->f) == 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) {
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
	if (s->type == SRC_OGG)
		ov_clear(&s->vf);
	else if (s->f)
		fclose(s->f);
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
	static const char *const ext[] = { ".ogg", ".wav", ".oga", "", NULL };
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
	int16_t *pcm; /* linear memory */
	long frames;
	int channels, rate;
	unsigned used; /* for evicting the least recently used */
} Sound;

#define SE_CACHE 48
static Sound sounds[SE_CACHE];
static unsigned use_clock;
static ndspWaveBuf se_wb[CH_SE_N];
static int next_se;

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
		slot->pcm = NULL;
	}
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
	if (audio_ok && linearSpaceFree() >= 2u * 1024 * 1024)
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
	ndspChnSetFormat(ch, snd->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetRate(ch, snd->rate * CLAMP(pitch, 50, 150) / 100.0f);
	set_mix(ch, sound_factor(vol));
	memset(&se_wb[c], 0, sizeof se_wb[c]);
	se_wb[c].data_pcm16 = snd->pcm;
	se_wb[c].nsamples = snd->frames;
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
