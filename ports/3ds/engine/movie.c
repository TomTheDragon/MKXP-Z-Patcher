/* Graphics.play_movie: Ogg Theora videos (.ogv, as RPG Maker VX Ace plays
 * them) with their Vorbis sound, played as they are (no conversion):
 * libtheora decodes the pictures, the 3DS's Y2R unit turns them into RGB
 * straight into a texture (8x8 tiles, as the GPU reads them: far too much
 * work for the processor otherwise), Tremor decodes the sound, which plays
 * on its own DSP channel and is the clock: a picture is shown when the
 * sound has reached its time; when decoding falls behind, pictures are
 * left out (decoded, not shown) until it's caught up. B skips the movie,
 * holding R resets the game. */
#include "engine.h"
#include <3ds.h>
#include <citro3d.h>
#include <ogg/ogg.h>
#include <theora/theoradec.h>
#include <tremor/ivorbiscodec.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
	FILE *f;
	ogg_sync_state sync;
	ogg_stream_state to, vo; /* theora, vorbis */
	bool has_t, has_v;
	th_info ti;
	th_comment tc;
	th_setup_info *ts;
	th_dec_ctx *td;
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;
	bool vorbis_ready;
} Movie;

/* the next page of the file into the streams; false at the end */
static bool next_page(Movie *m)
{
	ogg_page page;
	while (ogg_sync_pageout(&m->sync, &page) != 1) {
		char *buf = ogg_sync_buffer(&m->sync, 8192);
		size_t n = fread(buf, 1, 8192, m->f);
		if (!n)
			return false;
		ogg_sync_wrote(&m->sync, (long) n);
	}
	if (m->has_t)
		ogg_stream_pagein(&m->to, &page);
	if (m->has_v)
		ogg_stream_pagein(&m->vo, &page);
	return true;
}

/* the headers: which streams there are and their setup */
static bool open_movie(Movie *m, const char *path)
{
	memset(m, 0, sizeof *m);
	m->f = fopen(path, "rb");
	if (!m->f)
		return false;
	ogg_sync_init(&m->sync);
	th_info_init(&m->ti);
	th_comment_init(&m->tc);
	vorbis_info_init(&m->vi);
	vorbis_comment_init(&m->vc);
	int theora_headers = 0, vorbis_headers = 0;
	ogg_page page;
	ogg_packet op;
	/* the first pages: each stream's first header */
	for (;;) {
		while (ogg_sync_pageout(&m->sync, &page) != 1) {
			char *buf = ogg_sync_buffer(&m->sync, 8192);
			size_t n = fread(buf, 1, 8192, m->f);
			if (!n)
				return m->has_t;
			ogg_sync_wrote(&m->sync, (long) n);
		}
		if (!ogg_page_bos(&page)) {
			/* (data from here: into the streams) */
			if (m->has_t)
				ogg_stream_pagein(&m->to, &page);
			if (m->has_v)
				ogg_stream_pagein(&m->vo, &page);
			break;
		}
		ogg_stream_state test;
		ogg_stream_init(&test, ogg_page_serialno(&page));
		ogg_stream_pagein(&test, &page);
		ogg_stream_packetpeek(&test, &op);
		if (!m->has_t && th_decode_headerin(&m->ti, &m->tc, &m->ts, &op) >= 0) {
			memcpy(&m->to, &test, sizeof test);
			m->has_t = true;
			theora_headers = 1;
			ogg_stream_packetout(&m->to, &op);
		} else if (!m->has_v && vorbis_synthesis_headerin(&m->vi, &m->vc, &op) >= 0) {
			memcpy(&m->vo, &test, sizeof test);
			m->has_v = true;
			vorbis_headers = 1;
			ogg_stream_packetout(&m->vo, &op);
		} else {
			ogg_stream_clear(&test);
		}
	}
	/* the remaining headers (3 each) */
	while ((m->has_t && theora_headers < 3) || (m->has_v && vorbis_headers < 3)) {
		bool got = false;
		while (m->has_t && theora_headers < 3 && ogg_stream_packetpeek(&m->to, &op) == 1) {
			if (th_decode_headerin(&m->ti, &m->tc, &m->ts, &op) < 0)
				return false;
			ogg_stream_packetout(&m->to, &op);
			theora_headers++;
			got = true;
		}
		while (m->has_v && vorbis_headers < 3 && ogg_stream_packetpeek(&m->vo, &op) == 1) {
			if (vorbis_synthesis_headerin(&m->vi, &m->vc, &op) < 0)
				return false;
			ogg_stream_packetout(&m->vo, &op);
			vorbis_headers++;
			got = true;
		}
		if (!got && !next_page(m))
			return false;
	}
	if (!m->has_t)
		return false;
	m->td = th_decode_alloc(&m->ti, m->ts);
	if (!m->td)
		return false;
	int pp = 0; /* (no post-processing: less work) */
	th_decode_ctl(m->td, TH_DECCTL_SET_PPLEVEL, &pp, sizeof pp);
	if (m->has_v && vorbis_synthesis_init(&m->vd, &m->vi) == 0 && vorbis_block_init(&m->vd, &m->vb) == 0)
		m->vorbis_ready = true;
	return true;
}

static void close_movie(Movie *m)
{
	if (m->td)
		th_decode_free(m->td);
	th_setup_free(m->ts);
	th_comment_clear(&m->tc);
	th_info_clear(&m->ti);
	if (m->vorbis_ready) {
		vorbis_block_clear(&m->vb);
		vorbis_dsp_clear(&m->vd);
	}
	vorbis_comment_clear(&m->vc);
	vorbis_info_clear(&m->vi);
	if (m->has_t)
		ogg_stream_clear(&m->to);
	if (m->has_v)
		ogg_stream_clear(&m->vo);
	ogg_sync_clear(&m->sync);
	if (m->f)
		fclose(m->f);
}

/* sound decoded into the free DSP buffers; false at its end */
static bool feed_sound(Movie *m, bool *ended)
{
	if (!m->vorbis_ready) {
		*ended = true;
		return false;
	}
	int16_t *buf;
	bool any = false;
	while ((buf = audio_movie_buffer())) {
		int cap = audio_movie_buffer_frames(), n = 0, ch = m->vi.channels;
		while (n < cap) {
			ogg_int32_t **pcm;
			int got = vorbis_synthesis_pcmout(&m->vd, &pcm);
			if (got > 0) {
				int take = got < cap - n ? got : cap - n;
				for (int i = 0; i < take; i++)
					for (int c = 0; c < ch; c++) {
						int v = pcm[c][i] >> 9;
						buf[(n + i) * ch + c] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
					}
				vorbis_synthesis_read(&m->vd, take);
				n += take;
				continue;
			}
			ogg_packet op;
			if (ogg_stream_packetout(&m->vo, &op) == 1) {
				if (vorbis_synthesis(&m->vb, &op) == 0)
					vorbis_synthesis_blockin(&m->vd, &m->vb);
				continue;
			}
			if (!next_page(m)) {
				*ended = true;
				break;
			}
		}
		if (n > 0) {
			audio_movie_queue(n);
			any = true;
		}
		if (*ended)
			break;
	}
	return any;
}

/* --- Y2R: a decoded picture (Y, Cb, Cr planes) into the texture */
static uint8_t *planes[3]; /* linear copies of the planes (the Y2R unit reads them by DMA) */
static size_t plane_size[3];

static bool to_texture(th_ycbcr_buffer yuv, int w, int h, C3D_Tex *tex)
{
	/* the planes without their padding, in linear memory */
	int cw = yuv[1].width, chh = yuv[1].height;
	size_t need[3] = { (size_t) w * h, (size_t) cw * chh, (size_t) cw * chh };
	for (int i = 0; i < 3; i++) {
		if (plane_size[i] < need[i]) {
			linearFree(planes[i]);
			planes[i] = linearAlloc(need[i]);
			plane_size[i] = planes[i] ? need[i] : 0;
			if (!planes[i])
				return false;
		}
		int pw = i ? cw : w, ph = i ? chh : h;
		for (int y = 0; y < ph; y++)
			memcpy(planes[i] + (size_t) y * pw, yuv[i].data + (size_t) y * yuv[i].stride, pw);
		GSPGPU_FlushDataCache(planes[i], need[i]);
	}
	Y2RU_ConversionParams p = { 0 };
	p.input_format = cw * 2 == w && chh * 2 == h ? INPUT_YUV420_INDIV_8 : INPUT_YUV422_INDIV_8;
	p.output_format = OUTPUT_RGB_32;
	p.rotation = ROTATION_NONE;
	p.block_alignment = BLOCK_8_BY_8;
	p.input_line_width = (s16) w;
	p.input_lines = (s16) h;
	p.standard_coefficient = COEFFICIENT_ITU_R_BT_601;
	p.alpha = 0xff;
	Y2RU_StopConversion();
	if (R_FAILED(Y2RU_SetConversionParams(&p)))
		return false;
	Y2RU_SetSendingY(planes[0], (u32) need[0], (u16) w, 0);
	Y2RU_SetSendingU(planes[1], (u32) need[1], (u16) cw, 0);
	Y2RU_SetSendingV(planes[2], (u32) need[2], (u16) cw, 0);
	/* 8 rows at a time; the texture is wider than the picture */
	Y2RU_SetReceiving(tex->data, (u32) (w * h * 4), (u16) (w * 8 * 4), (u16) ((tex->width - w) * 8 * 4));
	Handle done;
	Y2RU_GetTransferEndEvent(&done);
	Y2RU_StartConversion();
	svcWaitSynchronization(done, 200000000LL);
	return true;
}

static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

/* the movie's file: the name, or with .ogv */
static bool find_movie(const char *name, char *out, size_t n)
{
	static const char *const ext[] = { "", ".ogv", ".ogg", NULL };
	return file_find(name, ext, out, n);
}

static VALUE g_play_movie(int argc, VALUE *argv, VALUE self)
{
	if (argc < 1)
		return Qnil;
	char path[512];
	const char *name = StringValueCStr(argv[0]);
	if (!find_movie(name, path, sizeof path)) {
		log_only("movie %s: not found\n", name);
		return Qnil;
	}
	Movie m;
	if (!open_movie(&m, path)) {
		log_only("movie %s: not a Theora video\n", path);
		close_movie(&m);
		return Qnil;
	}
	static bool y2r_ok, y2r_tried;
	if (!y2r_tried) {
		y2r_tried = true;
		y2r_ok = R_SUCCEEDED(y2rInit());
	}
	/* the frame (a multiple of 16) and the picture in it */
	int fw = (int) m.ti.frame_width, fh = (int) m.ti.frame_height;
	int pw = (int) m.ti.pic_width, ph = (int) m.ti.pic_height;
	C3D_Tex tex;
	bool tex_ok = y2r_ok && fw <= 1024 && fh <= 1024 && C3D_TexInit(&tex, (u16) pow2(fw), (u16) pow2(fh), GPU_RGBA8);
	if (!tex_ok) {
		log_only("movie %s: %dx%d not playable (Y2R %d)\n", path, fw, fh, y2r_ok);
		close_movie(&m);
		return Qnil;
	}
	C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
	memset(tex.data, 0, tex.size);
	double fps = m.ti.fps_denominator ? (double) m.ti.fps_numerator / m.ti.fps_denominator : 30;
	bool sound = m.vorbis_ready && audio_movie_open((int) m.vi.rate, m.vi.channels);
	log_only("movie %s: %dx%d (%dx%d shown), %.2f fps, sound %d\n", path, fw, fh, pw, ph, fps, sound);
	u64 t_start = svcGetSystemTick();
	long frame = 0, shown = 0, left_out = 0;
	bool sound_ended = !sound, video_ended = false, skip = false;
	ogg_int64_t granule = 0;
	double last_shown = -1;
	while (!skip && !(video_ended && (sound_ended || audio_movie_idle()))) {
		if (sound && !sound_ended)
			feed_sound(&m, &sound_ended);
		/* the time now: the sound's clock (else the system's) */
		double now = sound ? (double) audio_movie_played() / m.vi.rate
		                   : (double) (svcGetSystemTick() - t_start) / SYSCLOCK_ARM11;
		if (!video_ended) {
			ogg_packet op;
			if (ogg_stream_packetout(&m.to, &op) == 1) {
				if (th_decode_packetin(m.td, &op, &granule) == 0 || op.bytes == 0) {
					double t = frame / fps;
					frame++;
					/* late: not shown (but at least 4 a second, if decoding
					 * can't keep up at all) */
					if (t + 1.0 / fps < now && now - last_shown < 0.25) {
						left_out++;
					} else {
						last_shown = now;
						th_ycbcr_buffer yuv;
						th_decode_ycbcr_out(m.td, yuv);
						if (to_texture(yuv, fw, fh, &tex)) {
							/* waiting for its time */
							while (sound && !sound_ended && (double) audio_movie_played() / m.vi.rate < t) {
								feed_sound(&m, &sound_ended);
								svcSleepThread(2000000);
							}
							(void) pw;
							gpu_movie_frame(&tex, fw, fh);
							shown++;
						}
					}
				}
			} else if (!next_page(&m)) {
				video_ended = true;
			}
		} else {
			svcSleepThread(10000000);
		}
		input_poll();
		if (input_skip_movie())
			skip = true;
		if (!aptMainLoop())
			break;
		if (input_reset_hold() >= 1.0f)
			break;
	}
	log_only("movie %s: %ld of %ld pictures shown (%ld late)%s\n", path, shown, frame, left_out, skip ? " (skipped)" : "");
	if (sound)
		audio_movie_close();
	C3D_FrameSync();
	C3D_TexDelete(&tex);
	close_movie(&m);
	engine_check_reset();
	return Qnil;
}

void init_movie(void)
{
	VALUE mGraphics = rb_define_module("Graphics");
	rb_define_module_function(mGraphics, "play_movie", g_play_movie, -1);
}
