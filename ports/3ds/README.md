# Nintendo 3DS port (experiment)

Can mkxp-z run RPG Maker XP / VX / VX Ace games on a 3DS (homebrew)? This
folder holds the experiment, step by step. It isn't part of the patcher yet.

## Step 1: Ruby 3.1 on the 3DS (done)

`build.sh` builds mkxp-z's Ruby 3.1.3 for the 3DS with devkitPro's devkitARM
and libctru, and a test app, `build/rbtest.3dsx`, that starts Ruby and runs a
small benchmark (classes, Marshal, Zlib, fibers, hashes, GC).

Results in the Azahar emulator (2026-11-19):

- Ruby starts in about 0.6 s; memory in use after starting: about 6 MB
  (peak during the test: about 11 MB), so it fits the original 3DS too.
- Marshal, Zlib, fibers and the garbage collector work.
- Speed: `fib(22)` 70 ms, a 300,000-step loop 767 ms: roughly 50-100 times
  slower than a PC. The emulator's timing is only approximate; the New 3DS
  can run homebrew at 804 MHz (3 times the original 3DS).

What the 3DS C library lacks, and how it's handled:

- `ruby-compat/pipe.c`: `pipe()` and `poll()` (Ruby's thread scheduling uses
  them), in memory, as a device of devkitPro's C library; plus an `fcntl`
  wrapper for them (linked with `-Wl,--wrap=fcntl`).
- `ruby-compat/compat.c`: anonymous `mmap` from the heap (fiber stacks, no
  guard pages); stubs for users, processes, `waitpid`, `getrusage`,
  `pthread_kill`, `sigprocmask`.
- `ruby-compat/include/`: `sys/mman.h`, `poll.h`, and `mkxpz-3ds-compat.h`
  (included everywhere: `posix_signal`, `setmode`, `memrchr`).
- `ruby-3ds.patch`: no SIGSEGV handlers for GC compaction (`GC.compact`
  isn't usable), fewer pre-allocated fibers.
- Built with `-std=gnu17` (GCC 16 defaults to C23, which Ruby 3.1 predates),
  a 2 MB transient heap instead of 32 MB, and without the bigdecimal, ripper,
  socket, openssl, etc. extensions.
- Ruby must be started like mkxp-z does it (`ruby_init` + `ruby_options`),
  which also loads the built-in Ruby code (`Time.now` etc.).

Toolchain: copy `/opt/devkitpro` out of the official `devkitpro/devkitarm`
Docker image into `dist/devkitpro` (devkitPro's package server can't be used
by scripts). Build: `BASERUBY=<a Ruby 3.1 with psych> ports/3ds/build.sh`.

## Step 2: graphics (in progress)

`engine/` is a small RGSS engine for the 3DS (mkxp-z's renderer needs OpenGL
shaders, SDL2 and OpenAL, none of which the 3DS has): Ruby 3.1 with the RGSS
classes in C (`types.c` Rect/Color/Tone/Table with their Marshal formats,
`bitmap.c` with PNG loading, `font.c` text with FreeType, `scene.c`
Viewport/Sprite/Plane, `window.c` VX/VX Ace windows, `tilemap.c` VX/VX Ace
maps, `graphics.c`, `input.c`, `gpu.c`), `boot.rb` (Font, Kernel#load_data/
rgss_main, Audio and Win32API stand-ins) and mkxp-z's RPG data classes
(`ruby/module_rpg*.rb`, autotile tables in `autotiles.h`). `build.sh` also
builds `build/mkxpz.3dsx`; the game goes into `sdmc:/mkxpz` (or next to the
.3dsx). SELECT resets the game; A/B/X/Y/L/R/D-pad are RPG Maker's keys.

Drawing (`gpu.c`, citro3d): bitmaps become textures (uploaded when they
change, RGBA8 in the GPU's tiled layout); everything is a quad; opacity,
color and tone (gray via a dot product) in the six combiner stages; blending
normal/add/subtract; clipping with the scissor test. The scene is drawn at
the game's resolution into a 1024x512 texture and then scaled onto the top
screen with filtering (drawing it scaled directly loses 1-pixel lines, e.g.
window cursors); a second texture holds the frozen picture for
Graphics.freeze / transition (cross-fade). The tilemap keeps the visible
tiles in a vertex buffer (by layer and tileset bitmap) and only rebuilds it
when the visible tiles, the animation frame (every 30 frames), the map data
or the bitmaps change.

Results (2026-11-19, Azahar, original 3DS mode): Dazed (VX Ace) shows its
title (60 FPS) and, after WAKE, its first map with the player (about 40 FPS:
per frame the game's Ruby code needs 8.1 ms, preparing the drawing 6.9 ms,
and the rest is waiting for the GPU and the screen). The first software
renderer managed 12 FPS on the title.

Testing: `mkxpz-test.rb` in the game folder runs before the game's Main
script (e.g. to press buttons from Ruby); a file `gputest` shows a clipping
and orientation test picture for 5 seconds.

Not there yet: XP (RGSS1) windows and tilemaps, sprite waves, transition
pictures (plain cross-fade), Graphics.snap_to_bitmap draws in software
(without the map), flash_data, sound, the game's files in romfs.

Fixes found on the way: the 3DS C library's `fcntl` fails for files, and Ruby
aborts when `F_GETFD` fails with EBADF, so the `fcntl` wrapper answers the
simple requests itself.

## Next steps

- Real hardware numbers (old and New 3DS) decide how much more speed is
  needed: e.g. draw sprites from vertex buffers too, fewer state changes.
- Audio (ndsp: Ogg Vorbis with Tremor, WAV; MIDI is hard), RGSS1 (XP),
  romfs packaging, and a patcher option that makes a .3dsx / .cia.
