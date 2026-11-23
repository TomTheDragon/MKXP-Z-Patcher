/* MKXP-Z 3DS: sound for the 3DS, made on the PC (make-3ds-package.py
 * compiles this with the system's C compiler).
 *
 *   mkxa encode <rate> <channels> <loop start> <loop end> <out.mkxa> < pcm
 *       16-bit little-endian interleaved PCM -> .mkxa (DSP-ADPCM)
 *   mkxa midi <soundfont.sf2> <in.mid> <rate> > pcm
 *       MIDI rendered with TinySoundFont (as the engine would, at full
 *       quality); prints "loop <frame>" on stderr for RPG Maker's loop
 *       point (controller 111)
 *
 * .mkxa: "MKXA", u32 version (1), u32 rate, u32 channels, u32 frames (per
 * channel), u32 loop start, u32 loop end (0: none; the end), then per
 * channel 16 s16 coefficients (8 predictor pairs, 1/2048 units) and the
 * decoder's history before the frame with the loop start (s16 hist1,
 * hist2); then the data: DSP-ADPCM frames of 14 samples (8 bytes: a byte
 * predictor << 4 | scale, then 14 4-bit samples), the channels' frames
 * interleaved (frame 0 of each channel, frame 1 of each...). All little
 * endian. The 3DS's sound chip plays mono ones itself; the engine decodes
 * the others (cheap, unlike Vorbis or MP3). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSF_IMPLEMENTATION
#include "../engine/tsf.h"
#define TML_IMPLEMENTATION
#include "../engine/tml.h"

static int16_t clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t) v; }

/* --- the 8 predictors: least squares of each 14-sample frame, grouped
 * into 8 (k-means) */
static void find_coefs(const int16_t *x, long n, int16_t coefs[16])
{
	long nf = (n + 13) / 14;
	double *a = calloc(nf * 2, sizeof *a), *w = calloc(nf, sizeof *w);
	for (long f = 0; f < nf; f++) {
		double r11 = 0, r12 = 0, r22 = 0, r01 = 0, r02 = 0;
		for (long i = f * 14; i < f * 14 + 14 && i < n; i++) {
			double s0 = x[i], s1 = i > 0 ? x[i - 1] : 0, s2 = i > 1 ? x[i - 2] : 0;
			r11 += s1 * s1;
			r12 += s1 * s2;
			r22 += s2 * s2;
			r01 += s0 * s1;
			r02 += s0 * s2;
		}
		double det = r11 * r22 - r12 * r12, a1 = 0, a2 = 0;
		if (fabs(det) > 1e-3) {
			a1 = (r01 * r22 - r02 * r12) / det;
			a2 = (r02 * r11 - r01 * r12) / det;
		} else if (r11 > 1e-3) {
			a1 = r01 / r11;
		}
		if (a1 > 1.99) a1 = 1.99;
		if (a1 < -1.99) a1 = -1.99;
		if (a2 > 0.99) a2 = 0.99;
		if (a2 < -0.99) a2 = -0.99;
		a[f * 2] = a1;
		a[f * 2 + 1] = a2;
		w[f] = r11 + 1;
	}
	/* k-means, started from a fixed spread (as typical sets have) */
	double c[8][2] = { { 0, 0 }, { 1, 0 }, { 1.9, -0.95 }, { 1.5, -0.6 }, { 1.2, -0.3 }, { 0.5, 0 }, { 1.75, -0.8 },
	                   { 0.9, -0.2 } };
	for (int it = 0; it < 16 && nf > 0; it++) {
		double sum[8][2] = { { 0 } }, wsum[8] = { 0 };
		for (long f = 0; f < nf; f++) {
			int best = 0;
			double bd = 1e300;
			for (int k = 0; k < 8; k++) {
				double d1 = a[f * 2] - c[k][0], d2 = a[f * 2 + 1] - c[k][1], d = d1 * d1 + d2 * d2;
				if (d < bd) {
					bd = d;
					best = k;
				}
			}
			double ww = log(w[f]);
			sum[best][0] += a[f * 2] * ww;
			sum[best][1] += a[f * 2 + 1] * ww;
			wsum[best] += ww;
		}
		for (int k = 1; k < 8; k++) /* (predictor 0 stays "none") */
			if (wsum[k] > 0) {
				c[k][0] = sum[k][0] / wsum[k];
				c[k][1] = sum[k][1] / wsum[k];
			}
	}
	for (int k = 0; k < 8; k++) {
		coefs[k * 2] = clamp16((int) lround(c[k][0] * 2048));
		coefs[k * 2 + 1] = clamp16((int) lround(c[k][1] * 2048));
	}
	free(a);
	free(w);
}

/* one frame: the predictor and scale with the least error (tried all) */
static void encode_frame(const int16_t *x, int count, const int16_t coefs[16], int16_t *h1, int16_t *h2, uint8_t out[8])
{
	int best_err_set = 0;
	double best_err = 0;
	uint8_t best[8];
	int16_t bh1 = *h1, bh2 = *h2;
	for (int p = 0; p < 8; p++) {
		int c1 = coefs[p * 2], c2 = coefs[p * 2 + 1];
		for (int scale = 0; scale <= 12; scale++) {
			int s1 = *h1, s2 = *h2;
			double err = 0;
			uint8_t buf[8] = { (uint8_t) (p << 4 | scale) };
			for (int i = 0; i < 14; i++) {
				int sample = i < count ? x[i] : 0;
				int pred = c1 * s1 + c2 * s2;
				/* nibble: (sample << 11 - pred) / (2048 << scale), rounded */
				double q = ((double) sample * 2048 - pred - 1024) / (double) (2048 << scale);
				int nib = (int) lround(q);
				if (nib > 7) nib = 7;
				if (nib < -8) nib = -8;
				int dec = clamp16((((nib << scale) << 11) + 1024 + pred) >> 11);
				double e = (double) dec - sample;
				if (i < count)
					err += e * e;
				s2 = s1;
				s1 = dec;
				buf[1 + i / 2] |= (uint8_t) ((nib & 15) << (i & 1 ? 0 : 4));
			}
			if (!best_err_set || err < best_err) {
				best_err_set = 1;
				best_err = err;
				memcpy(best, buf, 8);
				bh1 = (int16_t) s1;
				bh2 = (int16_t) s2;
			}
		}
	}
	memcpy(out, best, 8);
	*h1 = bh1;
	*h2 = bh2;
}

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, int16_t v) { uint8_t b[2] = { (uint8_t) v, (uint8_t) ((uint16_t) v >> 8) }; fwrite(b, 1, 2, f); }

static int encode(int rate, int ch, long loop_start, long loop_end, const char *outname)
{
	/* the PCM from stdin */
	size_t cap = 1 << 20, n = 0;
	int16_t *pcm = malloc(cap * 2);
	size_t got;
	while ((got = fread(pcm + n, 2, cap - n, stdin)) > 0) {
		n += got;
		if (n == cap)
			pcm = realloc(pcm, (cap *= 2) * 2);
	}
	long frames = (long) (n / ch);
	if (loop_start < 0 || loop_start >= frames)
		loop_start = 0;
	if (loop_end <= loop_start || loop_end > frames)
		loop_end = 0;
	long nf = (frames + 13) / 14;
	int16_t coefs[2][16], hist[2][2] = { { 0 } };
	uint8_t *data = malloc((size_t) nf * ch * 8 + 8);
	int16_t *chan = malloc((size_t) (frames + 14) * 2);
	for (int c = 0; c < ch; c++) {
		for (long i = 0; i < frames; i++)
			chan[i] = pcm[i * ch + c];
		find_coefs(chan, frames, coefs[c]);
		int16_t h1 = 0, h2 = 0;
		long loop_frame = loop_start / 14;
		for (long f = 0; f < nf; f++) {
			if (f == loop_frame) {
				hist[c][0] = h1;
				hist[c][1] = h2;
			}
			long left = frames - f * 14;
			encode_frame(chan + f * 14, left < 14 ? (int) left : 14, coefs[c], &h1, &h2, data + ((size_t) f * ch + c) * 8);
		}
	}
	FILE *out = fopen(outname, "wb");
	if (!out) {
		perror(outname);
		return 1;
	}
	fwrite("MKXA", 1, 4, out);
	put32(out, 1);
	put32(out, rate);
	put32(out, ch);
	put32(out, (uint32_t) frames);
	put32(out, (uint32_t) loop_start);
	put32(out, (uint32_t) loop_end);
	for (int c = 0; c < ch; c++) {
		for (int k = 0; k < 16; k++)
			put16(out, coefs[c][k]);
		put16(out, hist[c][0]);
		put16(out, hist[c][1]);
	}
	fwrite(data, 8, (size_t) nf * ch, out);
	fclose(out);
	free(pcm);
	free(data);
	free(chan);
	return 0;
}

static int midi(const char *sf2, const char *mid, int rate)
{
	tsf *t = tsf_load_filename(sf2);
	tml_message *m = tml_load_filename(mid), *msg = m;
	if (!t || !m) {
		fprintf(stderr, "can't load %s\n", t ? mid : sf2);
		return 1;
	}
	tsf_set_output(t, TSF_STEREO_INTERLEAVED, rate, -3.0f);
	tsf_channel_set_bank_preset(t, 9, 128, 0);
	for (tml_message *p = m; p; p = p->next)
		if (p->type == TML_CONTROL_CHANGE && p->control == 111) {
			fprintf(stderr, "loop %ld\n", (long) ((long long) p->time * rate / 1000));
			break;
		}
	short buf[64 * 2];
	long pos = 0, tail = 0;
	for (;;) {
		unsigned ms = (unsigned) ((long long) pos * 1000 / rate);
		for (; msg && msg->time <= ms; msg = msg->next) {
			switch (msg->type) {
			case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(t, msg->channel, msg->program, msg->channel == 9); break;
			case TML_NOTE_ON: tsf_channel_note_on(t, msg->channel, msg->key, msg->velocity / 127.0f); break;
			case TML_NOTE_OFF: tsf_channel_note_off(t, msg->channel, msg->key); break;
			case TML_PITCH_BEND: tsf_channel_set_pitchwheel(t, msg->channel, msg->pitch_bend); break;
			case TML_CONTROL_CHANGE: tsf_channel_midi_control(t, msg->channel, msg->control, msg->control_value); break;
			}
		}
		if (!msg) {
			/* after the last message: the notes ringing out, at most 3 s */
			if (!tsf_active_voice_count(t) || tail > rate * 3)
				break;
			tail += 64;
		}
		tsf_render_short(t, buf, 64, 0);
		fwrite(buf, 4, 64, stdout);
		pos += 64;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 7 && !strcmp(argv[1], "encode"))
		return encode(atoi(argv[2]), atoi(argv[3]), atol(argv[4]), atol(argv[5]), argv[6]);
	if (argc == 5 && !strcmp(argv[1], "midi"))
		return midi(argv[2], argv[3], atoi(argv[4]));
	fprintf(stderr, "mkxa encode <rate> <channels> <loop start> <loop end> <out.mkxa> < pcm\n"
	                "mkxa midi <soundfont.sf2> <in.mid> <rate> > pcm\n");
	return 2;
}
