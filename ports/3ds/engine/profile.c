/* Sampling profiler for the game's Ruby code (a file "mkxpz-profile" in the
 * game folder turns it on). A thread wakes every 2 ms and asks Ruby to note
 * where it is (a postponed job: it runs at Ruby's next safe point, on the
 * main thread); every 600 frames the methods and lines that were seen most
 * go into mkxpz-3ds.log. Time spent in Graphics.update (drawing, waiting)
 * isn't sampled. Frames are told apart by their address and named when
 * first seen (the profiler keeps no Ruby objects: holding them across
 * minor GCs from C needs write barriers, and a freed one that was still
 * marked corrupted memory). */
#include "engine.h"
#include <3ds.h>
#include <ruby/debug.h>
#include <stdlib.h>
#include <string.h>

int rb_workqueue_register(unsigned flags, rb_postponed_job_func_t func, void *data); /* (Ruby: from any thread) */

#define NSLOT 4096
#define DEPTH 32
#define INTERVAL_NS 2000000LL

typedef struct {
	VALUE f;  /* frame (method or block), from rb_profile_frames: only its address */
	int line; /* -1: the whole method */
	unsigned self, total;
	VALUE callee; /* "called C" entries: the C method's frame */
	char *name; /* methods: "Class#label  (path)" */
} Slot;
#define CALLER 0x40000000 /* line flag: a Ruby line calling a C method */

bool prof_on;
static Slot *tab;
static unsigned nused, nsamples, nlost;
static volatile bool in_ruby, pending, quit;
static volatile u32 frame_no;
static u64 job_ticks;

/* appends a Ruby string (anything else: nothing) */
static void cat(char *buf, size_t n, VALUE v)
{
	if (!RB_TYPE_P(v, T_STRING))
		return;
	size_t l = strlen(buf), add = (size_t) RSTRING_LEN(v);
	if (l + add >= n)
		add = n - l - 1;
	memcpy(buf + l, RSTRING_PTR(v), add);
	buf[l + add] = 0;
}

/* the frame's name, while it is alive (in the job). Made of the parts
 * itself: rb_profile_frame_full_label formats with rb_sprintf, which calls
 * to_s on what isn't a string, and that crashed on the console. And
 * nothing here may make Ruby objects: the job runs at Ruby's interrupt
 * checks, e.g. before each file write while saving; when the job started
 * the garbage collector each time (> 2 ms), Ruby never got to the write
 * (the save hung). Labels, paths and method names are existing strings. */
static char *frame_name(VALUE f)
{
	if (!RB_TYPE_P(f, T_IMEMO))
		return strdup("?");
	char buf[200] = "";
	VALUE path = rb_profile_frame_path(f);
	bool singleton;
	cat(buf, sizeof buf, mkxpz_frame_class_name(f, &singleton));
	if (*buf)
		strcat(buf, singleton ? "." : "#");
	cat(buf, sizeof buf, NIL_P(path) ? rb_profile_frame_method_name(f) : rb_profile_frame_label(f));
	if (strlen(buf) + 8 < sizeof buf) {
		strcat(buf, "  (");
		if (NIL_P(path))
			strcat(buf, "C");
		else
			cat(buf, sizeof buf - 2, path);
		strcat(buf, ")");
	}
	return strdup(buf);
}

/* the Ruby line that called the C method running now (for the log) */
void prof_caller(char *out, size_t n)
{
	VALUE buf[16];
	int lines[16];
	int k = rb_profile_frames(0, 16, buf, lines), shown = 0;
	out[0] = 0;
	for (int i = 0; i < k && shown < 4; i++) {
		if (!RB_TYPE_P(buf[i], T_IMEMO) || NIL_P(rb_profile_frame_path(buf[i])))
			continue; /* (C methods: Bitmap#initialize, Class#new) */
		char *name = frame_name(buf[i]);
		size_t l = strlen(out);
		snprintf(out + l, n - l, "%sline %d in %s", shown ? " <- " : "", lines[i], name ? name : "?");
		free(name);
		shown++;
	}
	if (!shown)
		snprintf(out, n, "?");
}

static Slot *slot_c(VALUE f, int line, VALUE callee)
{
	unsigned h = ((unsigned) f >> 3) * 2654435761u ^ (unsigned) line * 40503u ^ ((unsigned) callee >> 3) * 97u;
	for (unsigned i = 0; i < 64; i++) {
		Slot *s = &tab[(h + i) & (NSLOT - 1)];
		if (s->f == f && s->line == line && s->callee == callee)
			return s;
		if (!s->f) {
			if (nused >= NSLOT * 3 / 4)
				return NULL;
			s->f = f;
			s->line = line;
			s->callee = callee;
			s->name = line < 0 ? frame_name(f) : NULL;
			nused++;
			return s;
		}
	}
	return NULL;
}
static Slot *slot(VALUE f, int line) { return slot_c(f, line, 0); }

static void sample_job(void *data)
{
	pending = false;
	if (!in_ruby || (u32) (uintptr_t) data != frame_no)
		return; /* asked before Graphics.update: not this frame's code */
	u64 t0 = svcGetSystemTick();
	VALUE buf[DEPTH];
	int lines[DEPTH];
	int n = rb_profile_frames(0, DEPTH, buf, lines);
	if (n > 0) {
		nsamples++;
		Slot *s = slot(buf[0], lines[0]), *m = slot(buf[0], -1);
		if (s)
			s->self++;
		if (m)
			m->self++;
		if (!s || !m)
			nlost++;
		/* a C method: which Ruby line called it */
		if (n > 1 && RB_TYPE_P(buf[0], T_IMEMO) && NIL_P(rb_profile_frame_path(buf[0]))) {
			Slot *c = slot_c(buf[1], lines[1] | CALLER, buf[0]);
			if (c)
				c->self++;
			if (!slot(buf[1], -1))
				nlost++;
		}
		for (int i = 0; i < n; i++) {
			bool seen = false;
			for (int j = 0; j < i && !seen; j++)
				seen = buf[j] == buf[i];
			Slot *t = seen ? NULL : slot(buf[i], -1);
			if (t)
				t->total++;
		}
	}
	job_ticks += svcGetSystemTick() - t0;
}

static void sampler(void *arg)
{
	while (!quit) {
		svcSleepThread(INTERVAL_NS);
		if (in_ruby && !pending) {
			pending = true;
			if (!rb_workqueue_register(0, sample_job, (void *) (uintptr_t) frame_no))
				pending = false;
		}
	}
}

void prof_init(void)
{
	FILE *f = fopen("mkxpz-profile", "r");
	if (!f)
		return;
	fclose(f);
	tab = calloc(NSLOT, sizeof *tab);
	if (!tab)
		return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	/* same core as the game, higher priority: it interrupts the game */
	if (!threadCreate(sampler, NULL, 32 * 1024, prio > 0x19 ? prio - 1 : 0x18, 0, true)) {
		say("profiler: no thread\n");
		return;
	}
	prof_on = true;
	in_ruby = true;
	say("profiler on (mkxpz-profile): results in mkxpz-3ds.log\n");
}

/* Graphics.update: start and end */
void prof_graphics_begin(void) { in_ruby = false; }
void prof_graphics_end(void)
{
	frame_no++;
	in_ruby = true;
}

static Slot *slot_find(VALUE f, int line)
{
	unsigned h = ((unsigned) f >> 3) * 2654435761u ^ (unsigned) line * 40503u;
	for (unsigned i = 0; i < 64; i++) {
		Slot *s = &tab[(h + i) & (NSLOT - 1)];
		if (!s->f)
			return NULL;
		if (s->f == f && s->line == line)
			return s;
	}
	return NULL;
}

static int by_self(const void *a, const void *b)
{
	const Slot *x = *(const Slot *const *) a, *y = *(const Slot *const *) b;
	return x->self != y->self ? (x->self < y->self ? 1 : -1) : (x->total < y->total ? 1 : x->total > y->total ? -1 : 0);
}
static int by_total(const void *a, const void *b)
{
	const Slot *x = *(const Slot *const *) a, *y = *(const Slot *const *) b;
	return x->total != y->total ? (x->total < y->total ? 1 : -1) : 0;
}

static void list(Slot **v, int n, int max, const char *what, bool lines, bool self)
{
	log_only("  %s:\n", what);
	for (int i = 0; i < n && i < max; i++) {
		Slot *s = v[i];
		if (!(self ? s->self : s->total))
			break;
		if (lines && s->callee) {
			Slot *m = slot_find(s->f, -1), *k = slot_find(s->callee, -1);
			log_only("    %5.1f%%  %s  from line %d in %s\n", 100.0 * s->self / nsamples, k && k->name ? k->name : "?",
			         s->line & ~CALLER, m && m->name ? m->name : "?");
		} else if (lines) {
			Slot *m = slot_find(s->f, -1);
			log_only("    %5.1f%%  line %d  in %s\n", 100.0 * s->self / nsamples, s->line, m && m->name ? m->name : "?");
		} else {
			log_only("    %5.1f%% self %5.1f%% with calls  %s\n", 100.0 * s->self / nsamples,
			         100.0 * s->total / nsamples, s->name ? s->name : "?");
		}
	}
}

/* into the log, then start again (frames: since the last report) */
void prof_report(int frames)
{
	if (!prof_on || !nsamples)
		return;
	Slot **m = malloc(sizeof *m * nused), **l = malloc(sizeof *l * nused), **cl = malloc(sizeof *cl * nused);
	int nm = 0, nl = 0, nc = 0;
	if (m && l && cl) {
		for (unsigned i = 0; i < NSLOT; i++) {
			if (!tab[i].f)
				continue;
			if (tab[i].line < 0)
				m[nm++] = &tab[i];
			else if (tab[i].callee)
				cl[nc++] = &tab[i];
			else
				l[nl++] = &tab[i];
		}
		log_only("profile: %u samples (every 2 ms of Ruby time), %u not counted; profiler %.2f ms per frame\n",
		         nsamples, nlost, (double) job_ticks * 1000.0 / SYSCLOCK_ARM11 / (frames ? frames : 1));
		qsort(m, nm, sizeof *m, by_self);
		list(m, nm, 25, "methods (self: in the method itself; with calls: and what it calls)", false, true);
		qsort(l, nl, sizeof *l, by_self);
		list(l, nl, 20, "lines", true, true);
		qsort(cl, nc, sizeof *cl, by_self);
		list(cl, nc, 15, "C methods (in C, from Ruby lines)", true, true);
		qsort(m, nm, sizeof *m, by_total);
		list(m, nm, 15, "methods with the most time including calls", false, false);
	}
	free(m);
	free(l);
	free(cl);
	for (unsigned i = 0; i < NSLOT; i++)
		free(tab[i].name);
	memset(tab, 0, sizeof *tab * NSLOT);
	nused = nsamples = nlost = 0;
	job_ticks = 0;
}
