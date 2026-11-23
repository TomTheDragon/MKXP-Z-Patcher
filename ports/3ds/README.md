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
.3dsx). SELECT resets the game; A/B/X/Y/R/D-pad are RPG Maker's keys, L the speed-up.

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
(without the map), flash_data, the game's files in romfs.

Fixes found on the way: the 3DS C library's `fcntl` fails for files, and Ruby
aborts when `F_GETFD` fails with EBADF, so the `fcntl` wrapper answers the
simple requests itself.

## Step 3: sound

`engine/audio.c` uses the 3DS's DSP (libctru's ndsp): BGM, BGS and ME are
streamed on their own channels (an audio thread decodes ahead into 4 buffers
of 4096 frames; on a New 3DS it runs on core 2, on the original 3DS on a 30%
share of the system core, so decoding costs the game nothing). Sound effects
are decoded once and cached (48 sounds), on a pool of 16 channels. Formats:
Ogg Vorbis (Tremor, with RPG Maker's LOOPSTART / LOOPLENGTH loop points) and
WAV (8/16-bit PCM); MIDI and MP3 are silent for now. An ME pauses the BGM,
which continues afterwards; playing the same BGM again only changes its
volume (games fade in that way); fades; bgm_pos / bgs_pos for resuming.

The DSP needs its firmware, `sdmc:/3ds/dspfirm.cdc` (made once on the console
by the "DSP1" homebrew); without it the game runs silently. The Azahar
emulator needs the file on its virtual SD card too (any file works there,
its DSP is emulated). Confirmed working on a real console and in Azahar.

Tested in Azahar (with its sound output off): Dazed's title (fire sounds
fading in, thunder effects), a 44.1 kHz stereo BGM at full speed on the map
(frame times unchanged), an ME pausing and resuming the BGM.

## Speed (original 3DS)

Measured in Azahar (original 3DS mode) on Dazed's first map:

- Text: letters are rendered once (FreeType is very slow here) and kept with
  their outline masks; blending without divisions. `draw_text` of a line
  with outline: 186 ms -> 4 ms; `text_size`: 81 ms -> 0.04 ms.
- Drawing: images that are fully transparent are skipped, Planes with
  power-of-two images are one quad (the GPU repeats the texture), the
  combiner setup is only redone when it changes: 7.0 -> 0.8 ms per frame
  (VX Ace's empty parallax Plane alone was 285 quads).
- Images: `tools/convert-images.py <game>` writes a `.mkxi` (RGBA, LZ4)
  next to each PNG, which the engine prefers: a 768x512 character sheet
  loads in 60-90 ms instead of 250-300. Folders are listed once
  (`files.c`), since every failed file open on the SD card costs ~20 ms.
- Ruby: built without stack protector and _FORTIFY_SOURCE: 8.2 -> 7.9 ms
  per frame (-O3 gave a little more in the emulator, which doesn't model the
  original 3DS's small caches, so it stays at -O2).
- Frame skip (settings): when a frame is more than one frame late, up to 3
  pictures in a row aren't drawn, so the game logic keeps its speed.

- Character sprites: `Sprite_Character#update` runs in C (end of
  `engine/scene.c`) when the standard VX Ace methods it stands for are
  unchanged. boot.rb compares their source text with checksums
  (`tools/fast-checksums.rb <game> --write` makes them) and asks again for
  each class it meets, so changed scripts keep the Ruby. Uncommon cases
  (animations, balloons, a new graphic) call the original Ruby methods. On
  Dazed's street map (108 events): 9.9 -> 4.6 ms per frame, identical
  results (tested by running both and comparing). `mkxpz-no-fast` in the
  game folder turns it off.
- Frame pacing: a late frame doesn't wait for the screen refresh; frames
  are only skipped when 2 behind and never 2 in a row (at least 30 drawn
  a second; when the scripts alone are too slow the game slows down a
  little instead). Street map: 60 drawn frames a second instead of 15.
- Memory (original 3DS: 64 MB for the game; Azahar gives 96 MB, so it
  hides memory problems): libctru's default split is a 24 MB heap and up
  to 32 MB of graphics ("linear") memory; the engine sets 20 MB graphics
  memory (`__ctru_linear_heap_size` in main.c), leaving ~35 MB of heap.
  RGSS's cache never frees bitmaps, so: textures not used lately are freed
  when graphics memory runs low (a 768x512 character sheet is a 2 MB
  texture; a street map uses ~11 MB per frame), and images loaded from
  files drop their pixels when the heap runs low and read them again when
  needed (bitmap.c). `.mkxi` files use 64 KB LZ4 blocks (`lz4 -B4`; the
  default 4 MB blocks need a 4 MB buffer to unpack, which failed on the
  console). `mkxpz-lowmem` in the game folder makes the emulator's normal
  memory as small as an original 3DS's, to test this. mkxpz-3ds.log gets
  the memory at startup, a speed line every 10 s and failed image loads.
- Tilemap: its vertex buffers cover 4 tiles more on each side, so they're
  rebuilt less often while scrolling.
- Pixel arena (memory.c; first real-console crash, 2026-11-20): after 4
  minutes of play, "failed to create bitmap (544x416): out of memory" with
  10 MB of the heap free: the free memory was in pieces between Ruby's
  objects. Image pixels of 64 KB and more now live in an arena of their
  own, taken from the heap once at startup (40%: about 13 MB on an
  original 3DS, next to ~19 MB for Ruby); freed images leave holes that
  merge again. When it's full, images not used lately let go of their
  pixels (only then does a buffer go into the normal heap). Tested in the
  emulator with the console's memory: 11 maps and all 47 pictures of
  Dazed, ~100 images let go and read again, nothing lost, 13 MB of the
  heap still free at the end. A heap that grows and shrinks with
  svcControlMemory (pixels in their own mappings) was tried first: in
  Azahar, writes to freshly mapped pages were sometimes lost (bytes missing
  from unpacked images), so memory is only arranged at startup.
- Profiler (profile.c): a file `mkxpz-profile` in the game folder samples
  the Ruby code every 2 ms (a thread asks Ruby for a postponed job, which
  notes the stack with `rb_profile_frames`); every 600 frames
  mkxpz-3ds.log gets the methods and lines seen most. It costs ~0.1 ms per
  frame. Frames are named when first seen and not kept as Ruby objects: a
  first version marked them from C, and a frame object freed in a minor GC
  was then still marked (it wrote into whatever had reused the memory).
  The speed line also has the garbage collector's time (`gc`).
- What it showed on Dazed's street map (108 events): the native
  `Sprite_Character#update` was 31% of the Ruby time, mostly reading
  instance variables (Ruby 3.1 looks each one up in a hash). It now
  remembers each variable's slot per call site (`IV()` in scene.c, for
  plain Ruby objects): 22%, results identical (11,615 sprite updates
  compared). Next in the list: `Game_CharacterBase#moving?` 8%,
  `update_stop` 5%, `Game_Map#loop_horizontal?/loop_vertical?` 9%,
  `Game_Event#refresh` with `conditions_met?` 13%.

- Second console run (2026-11-21, a later save): Ruby 30-42 ms per frame
  there; it crashed in the profiler (`rb_profile_frame_full_label` formats
  with `rb_sprintf`, which calls `to_s` on what isn't a string: data abort
  at 0x10 inside a postponed job). Frame names are now put together from
  the parts, each checked to be a String. The profile also lists C methods
  with the Ruby line that called them (to find who calls `Kernel#is_a?`,
  7-13% there).
- Big textures in pieces (gpu.c, up to 4x4): an axis is split into powers
  of two when one texture would be more than a quarter padding (544 ->
  512 + 32, 768 -> 512 + 256; 416 stays one 512). A 544x544 parallax needed
  one 1024x1024 texture (4 MB), which never fit on the console; now 1.1 MB.
  Bitmaps up to 4096 pixels are drawable (Dazed's 1536x1024 character
  sheets weren't shown at all before). Each piece is its own quad; checked
  with mirrored sprites (no seam). Tilemaps and repeating Planes still need
  single-piece textures.
- Faster native Sprite_Character#update: rbinternal.c (compiled with Ruby's
  internal headers) reads instance variables by cached slot also for the
  engine's C objects (their variables are in Ruby's generic table),
  `$game_map` by ID, and the animation checkers' class variables once per
  frame: street map 12.0 -> 10.9 ms of Ruby per frame in the emulator.

- Wide text boxes (settings: "Text boxes", on by default, with the fit
  screen mode): the picture is drawn 5:3, as wide as the top screen (692x416
  for a 544x416 game; `gpu_margin` shifts the scene projection and the
  scissor), the game draws in the middle as before, and only elements
  marked `mkxpz_wide` (Window/Sprite, no viewport) may use the sides.
  boot.rb marks VX/VX Ace's Window_Message (and its gold, choice, number and
  item windows, VX Ace's dimmed background) and makes it 2 x margin wider;
  VX Ace's choice list moves to the box's right end. Laid out again when the
  setting changes, once the box is closed.

- Main stack (third console run, 2026-11-21: the game quit without a word
  while saving): libctru gives the main thread 32 KB (`__stacksize__`), of
  which Ruby and the scenes used 22 KB right after starting, and Ruby
  didn't know the size (no getrlimit), so it never raised
  SystemStackError; Marshal.dump (saving) recurses in C, ~140 bytes per
  nesting level and more through Ruby marshal_dump methods. Now 512 KB
  (main.c), and ruby-3ds.patch tells Ruby (thread_pthread.c) so running out
  is an error message. The log's speed line shows the most stack ever used
  (`stack`, the unused part is filled with a pattern at startup) and how
  many bitmaps were made (`new bitmaps`; every 50th one's script line is
  logged, since the profiler attributes time in Bitmap.new to the first
  Ruby line after it: `Kernel#is_a?` in `Font#initialize`).

- The save crash, found with the user's save files: loading their Save02
  in the emulator, walking to the street map and saving from the menu hung
  inside Marshal.dump, at the same place as their half-written Save03 (the
  map's events). Only with the profiler: its postponed job named new frames
  with rb_profile_frame_classpath, which copies the class name (a Ruby
  object). The job runs at Ruby's interrupt checks, also the one before
  each file write; during the save each job started the garbage collector
  (> 2 ms), so a new job was always waiting and the write never happened.
  The job now makes no Ruby objects (class names read directly,
  `mkxpz_frame_class_name` in rbinternal.c).
- Hardening from the same search: the main stack (see above); image pixels
  are reported to Ruby's garbage collector (`mem_tell_ruby`, at safe
  points), and its thresholds fit the ~19 MB heap (4 MB instead of 16 MB
  allocated before it collects); stderr also goes into mkxpz-3ds.log (Ruby's
  "[FATAL] failed to allocate memory" when it quits).
- Stock VX Ace calls `Cache.picture` every frame for each picture, and for
  an erased one (empty name) that makes a new 32x32 bitmap every frame
  (`new bitmaps 60` on Dazed's maps).

- Still stopping on the console at the same byte (7230: the first 8 KB
  block of the file was written, the second never) with no dump and
  nothing on stderr. Save probe in boot.rb (`mkxpz-savelog`): converts each
  part of the save data (and each map event) on its own with logging, then
  saves through a wrapper that logs each block written.

- Found it: with the save probe the emulator hung too (intermittently),
  and Azahar's GDB stub (use_gdbstub in qt-config.ini, port 24689,
  devkitARM's arm-none-eabi-gdb) showed the main thread in Ruby's
  do_select -> libctru's select() -> our poll(), forever. Ruby 3.1 waits
  for a file to be writable with select() (poll() only on Linux) when a
  write was interrupted (a pending interrupt as it starts: e.g. the
  profiler's); libctru's select is for sockets and handed poll() something
  that never became ready. pipe.c now has its own select (linked with
  --wrap=select) on top of poll. SD writes also go through a guard
  (main.c): a write that writes nothing is retried, then fails with EIO,
  so Ruby can't loop on it; with mkxpz-savelog each write is logged.

- After saving worked: "FiberError: can't alloc machine stack to fiber (1 x
  331776 bytes)" (event interpreters run in fibers; 256 KB C stack + 64 KB
  Ruby stack each, from the fragmented heap). Now 128 KB C stack
  (RUBY_FIBER_MACHINE_STACK_SIZE, Ruby's 32-bit minimum) and the stacks come
  from the pixel arena (compat.c's mmap calls the engine's weak
  mkxpz_stack_alloc / mkxpz_stack_free). And a 1536x1024 sheet (6 MB) found
  no room with only 5 images in the arena: mem_release now lets go down to
  the most recent image when keeping 4 isn't enough.

- Built and tested in the emulator (2026-11-21 afternoon; ~45 min of game
  time on the user's Save03 with 61 saves, no errors):
  - Black title screen after a game over: Graphics.transition now sets the
    brightness back to 255, as RGSS and mkxp-z do (Scene_Gameover fades out
    with Graphics.fadeout).
  - Console crash after playing a while (Luma dump: data abort at 0x10 in
    Ruby's method lookup, inside `Font#initialize`'s `Color#dup` from
    `Bitmap.new`, the hottest allocation path: memory corruption). Suspect:
    an unreachable Sprite/Window stays in the engine's element list until
    Ruby frees it (lazily, in a postponed job), while its bitmap may already
    be freed; drawing it wrote into freed memory. collect() in scene.c now
    skips objects Ruby considers dead (rb_objspace_garbage_object_p).

- "failed to create bitmap (1536x1024)" (6 MB in one piece; the arena had
  8.8 MB free in 6 blocks). Now: unchanged file images of 256 KB or more let
  go of their pixels as soon as their texture is made (bitmap_uploaded;
  read again when needed); fiber stacks and bitmaps the game draws into come
  from the arena's top end, file images from the lowest hole that fits, so
  letting go of file images frees one connected area; while Bitmap.new
  loads a file every other file image may let go (nothing else is in use
  then; re-reads during drawing keep the most recent one); Bitmap#width,
  height and rect don't read dropped pixels again. Arena use on Dazed's
  maps: ~4 MB instead of ~10 MB. Three 1536x1024 sheets on screen in one
  frame need more textures than graphics memory holds (one or two fit).

- Native events: Game_Event#refresh with find_proper_page and
  conditions_met? (events.c) and Game_Event#update for events standing still
  (scene.c: animation and stop counters, the three update_stop,
  update_self_movement with near_the_screen?, check_event_trigger_auto;
  steps, pattern changes, move routes, start and the event's script call
  the Ruby methods), enabled by source checksums like Sprite_Character
  (the replaced methods themselves are checked once, before they're
  replaced). Tested with fixed random numbers and sound off (a scene in the
  user's save depends on the sound thread's timing): all 108 street events'
  state identical to the Ruby scripts at 40 checkpoints; Ruby time on the
  street 5.0 -> 4.0 ms (new game), 12.7 -> 7.5 ms (user's save) in the
  emulator. `mkxpz-no-fast-events` turns only this off.
- Speed display (settings: "Speed display"): game speed (game frames a
  second against 60), free/total RAM and graphics memory.
- The 1:1 screen mode is gone (fit and stretch).

- Sharp window text (settings: "Window text"): draw_text on a window's
  contents only records the text (string, a copy of the font, rect,
  alignment); window_draw hands the contents to gpu_text_layer, and after
  the scene is on the top screen the text is drawn at the screen's
  resolution from a text-only layer (font size x scale, positions x scale,
  light auto-hinting up to 18 px). New text is added to the layer, clear /
  clear_rect / fill_rect remove the texts they cover (redraw). The frozen
  picture of a transition gets the text at game size (a layer let go of
  after the freeze). Not covered: scripts reading window pixels, copying
  window contents elsewhere, something drawn above a window's text.
- The speed display is on the bottom screen's first line (its own console
  window; the messages scroll below it).

- Reset (SELECT): mkxpz_dispose_all disposes every sprite, window, plane,
  tilemap and viewport still registered, Graphics.mkxpz_reset restores the
  brightness and drops a pending transition, the audio stops (as mkxp-z
  does); before, a message box in the middle of its text stayed on screen.
- Smooth pictures (settings): the scene texture is presented with four
  bilinear samples a quarter of a screen pixel apart, averaged with the
  blend factors 1, 1/2, 1/3, 1/4 (a 58% shrink with one bilinear sample
  skips pixels: jagged edges). Mipmaps (C3D_TexGenerateMipmap) were tried
  first: the level stayed empty in Azahar (the picture went dark).
- Big .mkxi images: Bitmap.new only reads the size (>= 256 KB); textures
  are made straight from the file in blocks of 32 rows
  (bitmap_stream_file), outside of drawing (queued during a frame, read
  after it); the pixels are only loaded whole when a script needs them.
  No 6 MB block is needed for a 1536x1024 sheet any more.
- Screenshots in save files ("快速存储Bitmap的Marshal", e.g. Changed): the
  script's Bitmap#_dump / _load copy RGSS's memory with RtlMoveMemory;
  boot.rb replaces them (when the script defines them with its
  RtlMoveMemory constants) with the real pixels in the original format
  (BGRA, bottom row first, zlib), like the patcher's
  compat/bitmap_marshal.rb. Bitmap#raw_data / raw_data= (mkxp-z) too.
- Big images, tiled: convert-images.py writes images of 4 MB and more as
  .mkxi version 2 (256x256 tiles, each its own LZ4 frame, a table of
  offsets in front; transparent tiles not stored). Sparse textures use a
  fixed 256 grid (piece = tile, bitsets for up to 16x16 pieces), a piece
  comes from its tile alone (bitmap_tile); fully transparent pieces free
  their memory (empty). Version 1 files get random access through their
  independent 64 KB LZ4 blocks (bitmap_read_rows). Changed's boss fight:
  piece loads 30-150 ms -> under 10 ms (Azahar).
- Preloading: after Spriteset_Map#initialize, the character graphics of
  the map (current graphics first, then all event pages, move routes and
  "Set Move Route" / "Change Actor Graphic" in common events) get their
  texture pieces while 3 MB of linear memory are left.
- Kept textures: gpu_forget keeps the texture of an unchanged file image
  (named by the file) in the live list; the next Bitmap.new of the file
  takes it over (recycled_for); evict_one frees kept ones first. Images
  without pixels are read during the frame when first drawn (no deferral:
  pictures appeared a frame late), sparse pieces too. The preload also
  takes pictures (Show Picture) and sound effects (Play SE, move routes:
  Audio.mkxpz_preload_se, first and down to 2 MB free; images down to
  5 MB free). gfxSet3D before gfxSetWide (the other order turned wide
  mode off after 3D).
- $VERBOSE = nil while the game's scripts run (no "already initialized
  constant" warnings after a reset).
- 3D (settings "3D", slider > 0): gfxSet3D, a right-eye render target;
  the scene is drawn per eye with layers shifted (stereo_shift): planes
  behind, windows, viewports with z >= 50 and viewport-less sprites with
  z >= 100 in front (up to 4 game pixels per eye); the sharp window text
  too. Wide mode is off while 3D is.
- Last used save: boot.rb notes loads and saves through the standard
  routines (Scene_File / Scene_Load / Scene_Save read_save_data /
  write_save_data, DataManager.load_game / save_game_without_rescue) and
  File.mtime / File#mtime report that time for the file when later, so
  the games' "latest save" picks it. File times: stat / fstat on sdmc:
  get st_mtime from archive_getmtime (libctru leaves 0; Azahar answers
  2000-01-01 for every file).
- Bottom screen background: the .3dsx's SMDH icon (argv[0], or a .3dsx in
  the game folder), 4x, dimmed to 30%, drawn into dark pixels only after
  each console output. Settings are written only when they changed.
- Reset with R held for 2 s (input.c: shown after 0.3 s as "RESET" and a
  bar, gpu.c draw_reset_hold; R must be let go of before the next one).
  Graphics.mkxpz_reset draws one frame of the emptied scene (black, the
  load icon) so the last picture doesn't stay while the scripts reload.
- Graphics.snap_to_bitmap draws the scene on the GPU into scene texture 0
  in a frame of its own (window text at game size, as for a freeze; the
  top screen untouched), waits for the GPU (the next C3D_FrameBegin) and
  reads it back (tiled RGBA8, the picture's first row is the texture's
  last). The CPU compose it used before has no tilemap: save screenshots
  and menu backgrounds lacked the map.
- Save screens: Ruby patch (marshal.c): Marshal.load from a File opened
  only for reading loads from the rest of the file read once (kept with
  the File, hidden), then seeks to just after the object; EOFError at the
  end as before. Marshal.mkxpz_skip(file) steps over an object without
  making it (structure and class names checked). boot.rb replaces the
  "vx新截图存档" script's Window_SaveFile#load_gamedata (checksum) with one
  that loads parts 1-2, skips 12, loads the screenshot, cached per file
  (size + CRC of the first/last 4 KB). Bitmap#blur separable with exact
  reciprocal divisions; blt from a big file image stops reading after its
  rows. Changed, Save10 (670 KB): ~3.1 s -> 0.9 s, 0.22 s cached.
- "Top screen": auto (default; settings key top_screen=2), on, off. Auto:
  wide unless the original 2DS (CFGU_GetSystemModel) or an emulator:
  Citra-family emulators answer svcGetSystemInfo type 0x20000 (Azahar
  returns 2), a real 3DS returns an error there.
- Logging is off by default: mkxpz-3ds.log only with the setting "Log
  file" (or a file mkxpz-log in the game folder, for tests), the bottom
  screen's messages and speed lines only with "Bottom log" (settings key
  bottom_log; errors, "Press START" and the menu show anyway:
  console_force).
- Load/save icons (gpu.c draw_activity): loading_tick (image loading,
  streaming, sound decoding, load_data) shows a spinning 12x12 CD at the
  top right for 400 ms, writing_tick (every write to sdmc: except the
  log, main.c sd_write) a floppy disk for 1 s; after 150 ms without a
  frame an extra frame (the last picture with the icons) every 50 ms.
- Deferred texture uploads (big images during a frame): the texture is
  created only after the frame, together with its upload. Created during
  the frame and filled after it, textures came out empty after resets and
  loads (characters, tiles missing; found with a reset/load loop and
  Bitmap#mkxpz_tex_info). Texture pieces are at most 512 (1024 for axes
  over 2048): 1 MB blocks still fit a fragmented linear heap.
- Wide mode (settings "Top screen"): gfxSetWide, a 240x800 render target,
  scale_x doubled; window text rendered with letters twice as wide
  (FT_Set_Pixel_Sizes(2 * size, size), font.c text_xs); battery and
  loading bar drawn with a 400-wide projection. Not on the original 2DS
  (CFGU_GetSystemModel). Azahar shows only the left 400 columns.
- Speed display: drawn into the bottom framebuffer (rows 0-2, the console
  font at 2x for "Speed"), the message console below it.
- Drawing order as in RGSS2/3 and mkxp-z: by z, then (sprites) by y, then
  by creation (Element.sort_y); characters and objects of the same priority
  overlapped in creation order before.
- Frame skip: 0 off, 1..5 auto with at most that many skipped pictures in
  a row (the lateness that is still caught up grows with it); 2 by default
  (settings files carry `version=2`; older ones take the new defaults of
  frame skip and "Same sound" (4) once).
- Maps with many events (VX): character sprites far off the screen (their
  size and 64 pixels more) are only placed and hidden; Spriteset_Map's
  sprite loop, Game_CommonEvent#update without a parallel process and
  Game_Map#events_xy run in C when they are the standard methods (checksums
  in MKXPZ_STOCK_VX). C objects' instance variables are read through one
  table lookup per object (IvSet). Changed-special's cat boss (422 events):
  12.8 -> 8.8 ms of Ruby per frame in the emulator.
- Hooks into classes that the widescreen script also prepends to
  (Spriteset_Map, Window_NumberInput, Window_ChoiceList) are prepended
  modules, not aliases: after a reset an alias took the prepended method
  and the two called each other.
- Games in an RGSS archive (Game.rgssad / .rgss2a / .rgss3a):
  `tools/extract-archive.py <archive> [<folder>]` unpacks it (RGSSAD 1
  and 3, as the patcher's archive.rs) for the package; the engine reads
  only plain files. A package: game folder without the archive, plus the
  unpacked Data/Graphics, then convert-images.py and make-package.py.
- Pack (engine/pack.c): `tools/make-3ds-package.py [--icon <picture>]
  <game> <mkxpz.3dsx> <out>` makes the whole SD card folder: <Title>.3dsx,
  Game.ini, Fonts/, text files, and Game.data with everything else
  (archive unpacked, images only as .mkxi, no PNGs; no exe/dll/archive/
  project files/saves). Format: "MKXPZPK1", u32 count, u32 index length,
  index sorted by lowercase name (u32 offset, u32 size, u16 length, name),
  then the files. main.c's sdmc hooks (open/close/read/seek/stat/fstat)
  serve a read-only file from the pack when it isn't a plain file (plain
  files first: the game's own writes); files.c listings add the pack's
  names. The pack is read through sdmc's own functions under a lock.
- Memory: evict_one frees the oldest unit in linear memory: a whole
  texture or one sparse piece (piece_used), kept textures by age too.
  vram_migrate (frame start) moves stable textures (unchanged file images,
  kept ones) into VRAM with a GPU copy (VRAM_RESERVE kept free; the render
  targets have no depth buffer; vram_evict_all if a render target doesn't
  fit). Bitmaps whose texture is in VRAM get a new texture when changed.
  The map preload (gpu_preload_begin: a new frame number) makes room from
  textures older than it. Pixels needed again (bitmap_px reload) stay
  (keep_px); pixels of disposed unchanged file images up to 1 MB are kept
  (3 MB) and taken over by Bitmap.new. Pack: files read from Game.data
  without a failed SD open first (pack_first), except in folders that are
  plain too or written to.
- Sound: tools/convert-audio.py (make-3ds-package.py) turns Audio/* into
  .mkxa (tools/mkxa.c: DSP-ADPCM, 8 predictors per file by k-means, best
  predictor/scale per 14-sample frame; music resampled to 32728 Hz; Ogg
  LOOPSTART/LOOPLENGTH and MIDI controller 111 kept; MIDI rendered with
  TinySoundFont and an installed SoundFont). audio.c: .mkxa music decoded
  in the audio thread, mono .mkxa sound effects played by the DSP
  (NDSP_FORMAT_MONO_ADPCM), MP3 with libmpg123, MIDI with TinySoundFont
  (tsf.h patched to keep 16-bit samples; too slow on the 3DS for most
  pieces: the fallback). Sound effects cache: SE_BYTES.
- Movies (movie.c): Ogg Theora + Vorbis with libtheoradec / Tremor; Y2R
  converts each picture into a GPU_RGBA8 texture (8x8 blocks), the sound
  plays on its own DSP channel and is the clock; late pictures are left out
  (at least 4 a second shown). boot.rb's Win32API answers mciSendString
  (open / play / status / close) with Graphics.play_movie.
- Pictures, less memory, lossless: sparse pieces 128x128 (SPARSE_PIECE;
  PWORDS bit sets), convert-images.py writes 128 tiles and identical
  tiles once (same table entry; GpuTex.alias maps a piece to the first
  one with its tile); the .mkxi version word's second byte (0x80 set: a
  format decided) names the exact format (TEX_FMT: RGBA8, RGB8, LA8, L8,
  LA4, L4, A8, A4) used while the image is unchanged; 256 tiles of older
  packages still load (tiles a multiple of the piece). VRAM moves are a
  GPU TextureCopy (the CPU may not write VRAM on the console); inside a frame
  C3D_SyncTextureCopy only queues it, so the source is freed at the next
  frame's start (vram_src_free).
- Screen "extended": the PC patcher's compat/widescreen.rb (embedded by
  build.sh) with Graphics.widescreen_mode (2 when extended) and
  Graphics.widescreen_resize; wide_w 5:3 (704 at 416). RGSS_VERSION is
  defined for VX Ace. The start draws gpu_startup_progress until the first
  frame; the top framebuffers are cleared right after gfxInit.
- GPU command list: 512 KB, C3D_FrameSplit when 3/4 full (cmdbuf_room).
- Old games: boot.rb retries a section that doesn't compile with
  mkxpz_ruby18_fix (when x:, "foo (a, b)", |@ivar| block parameters,
  next/break outside a loop by the error's line), Ruby 1.8 classics
  (TRUE, Hash#index, Object#id, String#[]= with a number), Windows text
  mode (File.open "r" / File.read: universal_newline), MKXP-Z extras
  (System, Graphics.delta, Input.release?/time?/count and no-op keyboard
  / mouse / window calls), print as a message box (also RGSS1).
- pack.c: folder listings (sd dir hooks; .mkxi listed as .png) and a
  working folder for Dir.chdir into pack folders (pack_resolve). files.c
  is locked (the audio thread looks up music at the same time).
- Sound effects: at most 4 MB cached (SE_BYTES), released for textures.
- Text: ruby-3ds.patch loads plain Marshal strings as UTF-8 for XP / VX
  games (mkxpz_marshal_utf8; their data has no encodings). font.c takes a
  letter the font lacks from another font that has it (fallback_face).
- Display mode: libctru's gfxSet3D/gfxSetWide set one mode byte; only the
  wanted call is made.
- Tilemap bitmaps need one texture: gpu_tiles_begin remakes a split
  texture whole (Bitmap.whole; 512x384 A1/A2 were split 256+128 and the
  map stayed black).
- Fonts are found by FreeType's family, "family style", and the name
  table's Windows family (1) and full name (4) (RGSS games ask for the
  Windows names, e.g. "Noto Sans S Chinese Medium").
- Packaging: `tools/make-package.py [--icon <picture>] <game> <mkxpz.3dsx>
  [<out>]` writes `<Title>.3dsx` with the game's title (Game.ini) and
  Game.exe's icon (48x48) in the SMDH (replaced in place: the 3DSX
  extended header says where it is). The engine runs the game next to the
  .3dsx, so each game has its own folder and app.
- Sparse textures (gpu.c): images from files of 4 MB and more get their
  512x512 pieces only when drawn (draw_pieces marks them, sparse_fill
  reads them after the frame); pieces not drawn in the frame make room
  when the linear memory runs out. Changed's 3072x2048 character sheets
  (24 MB) need one 1 MB piece per shown cell; Dazed's 1536x1024 sheets
  dropped from 14.4 to 9.3 MB of textures on a crowded map.
- Sparse textures also for big images with their pixels in memory (pieces
  filled from the pixels; refilled when drawn into). VX windows blt from
  character sheets, which loaded their pixels; the whole-texture path then
  failed every frame and evicted everything else (missing graphics, slow).
  blt from a big image without pixels streams the needed rows from its
  file (Changed's 3072x2048 sheets don't fit into memory as pixels).
- Reset in RGSS1/2 games (no rgss_main): mkxpz_run_all runs the scripts
  again from inside the rescue ($! / $@ set, as scripts' "alias ... unless
  $@" expect); the reset disposes with the engine's own dispose methods
  (the game's overrides disposed windows twice).
- romfs stays mounted at exit (FreeType keeps its fonts open; the C
  library's final fclose crashed after romfsExit).
- VX (RGSS2) speed-ups: Sprite_Character#update in C (scene.c
  fast_update_vx, MKXPZ_STOCK_VX checksums from VX's standard scripts),
  "Anti Lag VX"'s Game_Event#on_screen in C (events.c). Method source for
  the checksums is also read from script files (loader games). Ruby patch
  (ruby-3ds.patch): Integer == nil/true/false/symbol is false at once
  (num_equal and the VM's opt_eq/opt_neq) instead of going through the
  recursion guard. Changed's lab map (269 events, Azahar): 24.5 -> 9.8 ms
  of Ruby per frame; the console was about 110 ms before.
- More VX in C: Game_Event#update for standing events (scene.c
  fast_event_update_vx, installed under each name holding VX's standard
  method, e.g. "Anti Lag VX"'s pre_antilag_update), "Anti Lag VX"'s
  Game_Event#update / update? and Spriteset_Map#update_characters, VX's
  Game_Map#update_events and Game_Interpreter#setup_starting_event
  (events.c; the native updates called directly for classes boot.rb
  confirms). IVSET: instance variables set in their cached slot.
  Checked with seeded runs (native against mkxpz-no-fast: the events'
  state, variables and switches every 60 frames identical over 1800
  frames). Changed's boss room (457 events): 7.8-11.1 -> 3.7-4.9 ms.
- Games without a "Main" section (scripts from files through a loader,
  e.g. Changed with the RGSS Script Editor loader): boot.rb's preparations
  (and mkxpz-test.rb) run at the first Graphics.update / freeze /
  transition instead (Graphics.mkxpz_setup_later).
- Tested with a second game: Changed (VX, 2026-11-21): title, intro, mode
  choice, saving and loading, walking, messages at full speed in Azahar.
- Battery icon (battery.c, drawn last in gpu_present): mcu::HWC's
  percentage when it fits ptm:u's 0..5 step (Azahar's MCU answers with
  the step), else the step; asked every 2 s; charging from ptm:u.
- Buttons (default): A confirm, B cancel, Y run, X / R page up / down,
  ZL / ZR the X / Y keys, L held = speed-up (no waiting for the screen,
  a picture at most 15 times a second by the clock, the rest of the time
  runs the game: about 330% in Changed's boss fight in the emulator); settings "Buttons" has the old layout.
- The speed display is drawn again after every other console output
  (libctru's scrolling of a console window wiped the line next to it:
  flicker).
- The profiler (mkxpz-profile) was involved in all three "random" console
  crashes (its job sampling while Ruby builds an exception); it's for
  diagnosis only and no longer in the test package.

The game's own Ruby code is now most of the time. On a real original 3DS
(2026-11-20, Dazed) it was 15-18 ms per frame on the maps (the emulator
shows about half), with drawing 2-4 ms: the game ran at 45-55 frames a
second.

Settings (START in the game, saved in `mkxpz-settings.ini` in the game
folder): music / sound volume, buttons (B confirm, A cancel, Y dash, or
A/B/X), screen (fit, stretch), colors (normal, vivid: more
contrast for the original 3DS's pale screen), frame skip, show speed, reset.

## Next steps

- A profile from the real console (`mkxpz-profile`) decides what goes
  native next: likely Game_CharacterBase's per-frame methods for events
  (with the same source-checksum check as Sprite_Character).
- MIDI (XP/VX games' RTP music: a small SoundFont synthesizer would be
  heavy on the original 3DS), MP3 (libmpg123 is there), RGSS1 (XP), romfs
  packaging, and a patcher option that makes a .3dsx / .cia.
