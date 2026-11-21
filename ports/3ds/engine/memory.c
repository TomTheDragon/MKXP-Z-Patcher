/* Memory for image pixels: an arena of their own.
 *
 * Ruby's objects and the images' pixels used to share the heap, and after
 * a while its free memory was in pieces between Ruby's objects: 10 MB free
 * in total, but no single 0.9 MB block for a picture (seen on a real
 * original 3DS: "failed to create bitmap (544x416)"). So pixel buffers of
 * 64 KB and more get an arena that is taken from the heap once at startup
 * (40%: about 15 MB on an original 3DS) and holds nothing else: freed
 * images leave holes that merge again, and Ruby can't fill them with small
 * objects. When the arena is full, images not used lately let go of their
 * pixels (bitmap.c: mem_release) until the new one fits; only then does a
 * buffer go into the normal heap.
 *
 * (A heap that grows and shrinks with svcControlMemory, pixels in their own
 * mappings, was tried first: in the emulator writes to freshly mapped pages
 * were sometimes lost, so the memory is only arranged once, at startup.) */
#include "engine.h"
#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GRAIN 4096u /* arena blocks are multiples of this */
#define ROUND(n) (((n) + GRAIN - 1) & ~(GRAIN - 1))

static char *arena;
static size_t arena_size;
unsigned mem_px_maps;  /* pixel buffers in the arena (for the log) */
size_t mem_stack_bytes; /* of which fiber stacks */
/* pixel memory allocated (+) and freed (-) since Ruby was last told: its
 * garbage collector runs sooner when much memory was allocated, and pixel
 * buffers aren't Ruby's own allocations. Told only at safe points
 * (mem_tell_ruby), because that may start the collector right away. */
static long untold;

void mem_tell_ruby(void)
{
	long d = untold;
	untold = 0;
	if (d)
		rb_gc_adjust_memory_usage(d);
}
size_t mem_px_bytes;

typedef struct { u32 a, n; } Range;
static Range holes[512]; /* free parts of the arena */
static int nholes;

size_t heap_free(void)
{
	/* not yet taken from the system (libctru's heap ends at fake_heap_end),
	 * and malloc's free blocks */
	extern char *fake_heap_end;
	char *top = sbrk(0);
	struct mallinfo mi = mallinfo();
	return (size_t) (fake_heap_end > top ? fake_heap_end - top : 0) + mi.fordblks;
}

/* the arena: a share of the free heap (once, after Ruby has started) */
void mem_init(void)
{
	size_t total = heap_free();
	size_t want = total / 100 * 40;
	if (want > 48u * 1024 * 1024)
		want = 48u * 1024 * 1024;
	want &= ~(GRAIN - 1);
	while (want >= 2u * 1024 * 1024 && !(arena = memalign(GRAIN, want)))
		want -= 1024 * 1024;
	if (!arena)
		return;
	arena_size = want;
	holes[0] = (Range) { (u32) arena, (u32) want };
	nholes = 1;
	log_only("memory: %lu KB for images, %lu KB for the rest\n", (unsigned long) (want / 1024),
	         (unsigned long) (heap_free() / 1024));
}

size_t mem_arena_free(void)
{
	size_t n = 0;
	for (int i = 0; i < nholes; i++)
		n += holes[i].n;
	return n;
}

static bool in_arena(const void *p)
{
	return arena && (const char *) p >= arena && (const char *) p < arena + arena_size;
}

static void *arena_take(size_t bytes)
{
	u32 n = ROUND(bytes);
	/* the lowest hole that fits: images from files pack at the bottom (the
	 * ones that stay come from the top: arena_take_high) */
	int best = -1;
	for (int i = 0; i < nholes; i++)
		if (holes[i].n >= n && (best < 0 || holes[i].a < holes[best].a))
			best = i;
	if (best < 0)
		return NULL;
	u32 a = holes[best].a;
	holes[best].a += n;
	holes[best].n -= n;
	if (!holes[best].n)
		holes[best] = holes[--nholes];
	mem_px_maps++;
	mem_px_bytes += n;
	return (void *) a;
}

/* from the top end of the highest hole that fits: for fiber stacks, which
 * live long and can't be let go of, so they don't split the space images
 * need (those come from the best-fitting hole, mostly lower down) */
static void *arena_take_high(size_t bytes)
{
	u32 n = ROUND(bytes);
	int best = -1;
	for (int i = 0; i < nholes; i++)
		if (holes[i].n >= n && (best < 0 || holes[i].a > holes[best].a))
			best = i;
	if (best < 0)
		return NULL;
	holes[best].n -= n;
	u32 a = holes[best].a + holes[best].n;
	if (!holes[best].n)
		holes[best] = holes[--nholes];
	mem_px_maps++;
	mem_px_bytes += n;
	return (void *) a;
}

static void arena_give(void *p, size_t bytes)
{
	u32 a = (u32) p, n = ROUND(bytes);
	mem_px_maps--;
	mem_px_bytes -= n;
	/* merge with the neighbours (holes never touch each other) */
	for (int i = 0; i < nholes; i++) {
		if (holes[i].a + holes[i].n == a) {
			a = holes[i].a;
			n += holes[i].n;
			holes[i--] = holes[--nholes];
		} else if (a + n == holes[i].a) {
			n += holes[i].n;
			holes[i--] = holes[--nholes];
		}
	}
	if (nholes < (int) (sizeof holes / sizeof holes[0]))
		holes[nholes++] = (Range) { a, n };
	/* (else that part of the arena is lost; 512 holes don't happen) */
}

/* pixels (bytes = w * h * 4); zero: cleared; NULL: no memory even after
 * letting go of images not used lately */
static void *arena_take_high(size_t bytes);

void *px_alloc(size_t bytes, bool zero)
{
	return px_alloc_at(bytes, zero, false);
}

/* keep: memory that can't be let go of (bitmaps the game draws into): from
 * the arena's top end, like the fiber stacks; images read from files (which
 * can always be read again) from the best-fitting hole, mostly at the
 * bottom. Letting go of file images then frees one connected area (the
 * console once had 8.8 MB free in pieces and no 6 MB block) */
void *px_alloc_at(size_t bytes, bool zero, bool keep)
{
	void *p = NULL;
	if (bytes >= PX_MAP_MIN && arena) {
		void *(*take)(size_t) = keep ? arena_take_high : arena_take;
		p = take(bytes);
		for (int tries = 0; !p && tries < 256 && mem_release(bytes, true); tries++)
			p = take(bytes);
		if (p) {
			if (zero)
				memset(p, 0, bytes);
			untold += (long) bytes;
			return p;
		}
	}
	p = zero ? calloc(1, bytes) : malloc(bytes);
	for (int tries = 0; !p && tries < 256 && mem_release(bytes, false); tries++)
		p = zero ? calloc(1, bytes) : malloc(bytes);
	if (p)
		untold += (long) bytes;
	return p;
}

void px_free(void *p, size_t bytes)
{
	if (!p)
		return;
	untold -= (long) bytes;
	if (in_arena(p))
		arena_give(p, bytes);
	else
		free(p);
}

bool px_in_arena(const void *p) { return p && in_arena(p); }

/* Ruby's fiber stacks (ruby-compat's mmap; each ~196 KB, event scripts and
 * message boxes run in fibers): from the arena too. In the heap they once
 * found no free block that big after a long play (FiberError). */
void *mkxpz_stack_alloc(size_t bytes)
{
	if (!arena)
		return NULL;
	void *p = arena_take_high(bytes);
	for (int tries = 0; !p && tries < 256 && mem_release(bytes, true); tries++)
		p = arena_take_high(bytes);
	if (p)
		mem_stack_bytes += ROUND(bytes);
	return p;
}

bool mkxpz_stack_free(void *p, size_t bytes)
{
	if (!p || !in_arena(p))
		return false;
	mem_stack_bytes -= ROUND(bytes);
	arena_give(p, bytes);
	return true;
}
