/* MKXP-Z 3DS feasibility test: Ruby 3.1 on the 3DS */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <ruby.h>
#include <ruby/encoding.h>

void Init_ext(void);
void Init_enc(void);

static const char *SCRIPT =
"$stdout.sync = true\n"
"t0 = Time.now\n"
"puts \"Ruby #{RUBY_VERSION} (#{RUBY_PLATFORM})\"\n"
"class Game_Actor\n"
"  attr_accessor :name, :hp, :items\n"
"  def initialize(n) @name = n; @hp = 100; @items = Array.new(50) { |i| [i, \"item#{i}\"] } end\n"
"end\n"
"actors = (1..200).map { |i| Game_Actor.new(\"Actor #{i}\") }\n"
"data = Marshal.dump(actors)\n"
"back = Marshal.load(data)\n"
"puts \"Marshal: #{data.bytesize} bytes, #{back.size} actors, #{back[7].items[3][1]}\"\n"
"z = Zlib::Deflate.deflate(data)\n"
"puts \"Zlib: #{z.bytesize} bytes, inflate ok: #{Zlib::Inflate.inflate(z) == data}\"\n"
"def fib(n) n < 2 ? n : fib(n - 1) + fib(n - 2) end\n"
"t = Time.now; f = fib(22); puts \"fib(22) = #{f} in #{((Time.now - t) * 1000).round} ms\"\n"
"t = Time.now; s = 0; 300_000.times { |i| s += i % 7 }; puts \"300k loop: #{((Time.now - t) * 1000).round} ms\"\n"
"h = {}; 20_000.times { |i| h[\"k#{i}\"] = i * 2 }; puts \"hash: #{h.size} entries\"\n"
"puts \"utf-8: #{'Ümlaut äöü'.upcase} / #{'日本語'.length} chars\"\n"
"f = Fiber.new { Fiber.yield 1; 2 }; puts \"fiber: #{f.resume} #{f.resume}\"\n"
"GC.start\n"
"puts \"GC: #{GC.stat[:heap_live_slots]} live objects, #{GC.count} runs\"\n"
"puts \"total: #{((Time.now - t0) * 1000).round} ms\"\n";

static void memory(const char *when)
{
	struct mallinfo mi = mallinfo();
	printf("[%s] heap in use: %d KB (arena %d KB)\n", when, mi.uordblks / 1024, mi.arena / 1024);
}

static int finished;

/* an early exit() (e.g. from Ruby's startup): keep the message on screen */
static void on_exit_wait(void)
{
	if (finished)
		return;
	printf("\n*** exit() called early; waiting 60 s ***\n");
	for (int i = 0; i < 60 * 60 && aptMainLoop(); i++) {
		gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
	}
}

int main(int argc, char **argv)
{
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	atexit(on_exit_wait);
	printf("step: sysinit\n");
	printf("MKXP-Z 3DS test: Ruby\napp memory: %lu KB\n", (unsigned long) osGetMemRegionSize(MEMREGION_APPLICATION) / 1024);
	memory("start");
	u64 t = osGetTime();
	ruby_sysinit(&argc, &argv);
	{
		RUBY_INIT_STACK;
		printf("step: ruby_setup\n");
		int setup = ruby_setup();
		if (setup) {
			VALUE err = rb_errinfo();
			printf("ruby_setup failed (state %d)\n", setup);
			if (!NIL_P(err)) {
				VALUE s = rb_inspect(err);
				printf("%s\n", StringValueCStr(s));
				VALUE bt = rb_funcall(err, rb_intern("backtrace"), 0);
				if (!NIL_P(bt)) {
					VALUE j = rb_ary_join(bt, rb_str_new_cstr("\n"));
					printf("%s\n", StringValueCStr(j));
				}
			}
			on_exit_wait();
			finished = 1;
			return 1;
		}
		/* like mkxp-z: ruby_options loads the built-in Ruby code, encodings and extensions */
		printf("\nstep: ruby_options\n");
		char *rargs[] = {"mkxp-z", "-e ", NULL};
		void *node = ruby_options(2, rargs);
		int st = 0;
		if (!ruby_executable_node(node, &st))
			printf("ruby_options failed (state %d)\n", st);
		rb_enc_set_default_external(rb_enc_from_encoding(rb_utf8_encoding()));
		printf("Ruby started in %llu ms\n", osGetTime() - t);
		memory("Ruby up");
		int state = 0;
		rb_eval_string_protect("require 'zlib'", &state);
		rb_eval_string_protect(SCRIPT, &state);
		if (state) {
			VALUE err = rb_errinfo();
			VALUE msg = rb_funcall(err, rb_intern("full_message"), 0);
			printf("Error: %s\n", StringValueCStr(msg));
		}
		memory("after script");
	}
	printf("Done. Press START to exit.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
	}
	finished = 1;
	gfxExit();
	return 0;
}
