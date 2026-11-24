#!/bin/sh
# MKXP-Z 3DS port, step 1: Ruby 3.1 (mkxp-z's, with its patches) for the 3DS,
# and a test app (ruby-test/ -> build/rbtest.3dsx) that starts it and runs a
# small Ruby benchmark.
#
#   ports/3ds/build.sh
#
# Settings (environment):
#   DEVKITPRO  devkitPro folder (default: dist/devkitpro, copied out of the
#              devkitpro/devkitarm Docker image)
#   RUBY_SRC   mkxp-z's Ruby 3.1.3 source, already patched by meson
#              (default: dist/mkxp-z_source/subprojects/ruby-3_1_3)
#   BASERUBY   a Ruby 3.1 for this computer (with psych), needed to build Ruby
#   BUILD      build folder (default: ports/3ds/build)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DEVKITPRO=${DEVKITPRO:-$ROOT/dist/devkitpro}
RUBY_SRC=${RUBY_SRC:-$ROOT/dist/mkxp-z_source/subprojects/ruby-3_1_3}
BASERUBY=${BASERUBY:-$(command -v ruby)}
BUILD=${BUILD:-$HERE/build}
C=$HERE/ruby-compat
export PATH=$DEVKITPRO/devkitARM/bin:$DEVKITPRO/tools/bin:$PATH
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"

# Ruby's source, with the 3DS changes (no SIGSEGV handlers for GC compaction)
mkdir -p "$BUILD"
if [ ! -f "$BUILD/src/.mkxpz-3ds" ]; then
    rm -rf "$BUILD/src"
    cp -r "$RUBY_SRC" "$BUILD/src"
    (cd "$BUILD/src" && patch -p1 < "$HERE/ruby-3ds.patch")
    touch "$BUILD/src/.mkxpz-3ds"
fi

# configure: what the 3DS C library has is answered here where it can't be
# tested (cross build); poll/pipe come from ruby-compat/pipe.c
if [ ! -f "$BUILD/ruby/Makefile" ]; then
    mkdir -p "$BUILD/ruby"
    (cd "$BUILD/ruby" && ../src/configure --host=arm-none-eabi --build=x86_64-pc-linux-gnu \
        ac_cv_func_waitpid=yes ac_cv_header_poll_h=yes ac_cv_func_poll=yes \
        rb_cv_binary_elf=no ac_cv_func_dladdr=no ac_cv_header_dlfcn_h=no \
        stack_protector=no --disable-fortify-source \
        CC=arm-none-eabi-gcc AR=arm-none-eabi-ar RANLIB=arm-none-eabi-ranlib \
        CFLAGS="$ARCH -std=gnu17 -O2 -ffunction-sections -fdata-sections" \
        CPPFLAGS="-D__3DS__ -DTRANSIENT_HEAP_TOTAL_SIZE=2097152 -I$C/include -include mkxpz-3ds-compat.h -I$DEVKITPRO/libctru/include -I$DEVKITPRO/portlibs/3ds/include" \
        LDFLAGS="-specs=3dsx.specs $ARCH -L$DEVKITPRO/libctru/lib -L$DEVKITPRO/portlibs/3ds/lib" LIBS="-lctru" \
        --prefix="$BUILD/install" --disable-shared --enable-static --with-static-linked-ext \
        --disable-install-doc --disable-rubygems --with-coroutine=arm32 --with-baseruby="$BASERUBY" \
        --without-gmp --disable-jit-support --disable-dtrace \
        --with-out-ext=bigdecimal,ripper,rbconfig/sizeof,socket,openssl,readline,pty,syslog,etc,fiddle,dbm,gdbm,win32,win32ole,nkf,racc/cparse,objspace,io/console,io/wait,io/nonblock,fcntl)
fi
make -C "$BUILD/ruby" -j"$(nproc)"

# the test app
R=$BUILD/ruby
RS=$BUILD/src
T=$BUILD/test
mkdir -p "$T"
CF="$ARCH -std=gnu17 -O2 -D__3DS__ -I$C/include -I$DEVKITPRO/libctru/include"
arm-none-eabi-gcc $CF -include mkxpz-3ds-compat.h -I"$R/.ext/include/arm-eabi" -I"$RS/include" \
    -c "$HERE/ruby-test/main.c" -o "$T/main.o"
arm-none-eabi-gcc $CF -c "$C/compat.c" -o "$T/compat.o"
arm-none-eabi-gcc $CF -c "$C/pipe.c" -o "$T/pipe.o"
EXTS=$(cd "$R" && find ext enc -name '*.a' ! -name extinit.a ! -name encinit.a | sed "s|^|$R/|")
arm-none-eabi-gcc -specs=3dsx.specs $ARCH -Wl,--gc-sections -Wl,--wrap=fcntl -Wl,--wrap=select -o "$T/rbtest.elf" \
    "$T/main.o" "$T/compat.o" "$T/pipe.o" "$R/ext/extinit.o" "$R/enc/encinit.o" \
    -Wl,--start-group "$R/libruby-static.a" $EXTS -Wl,--end-group \
    -L"$DEVKITPRO/libctru/lib" -L"$DEVKITPRO/portlibs/3ds/lib" -lz -lm -lctru
smdhtool --create "MKXP-Z Ruby test" "Ruby 3.1 on the 3DS" "MKXP-Z Patcher" \
    "$DEVKITPRO/libctru/default_icon.png" "$T/rbtest.smdh"
3dsxtool "$T/rbtest.elf" "$BUILD/rbtest.3dsx" --smdh="$T/rbtest.smdh"
echo "Built $BUILD/rbtest.3dsx"

# --- the engine (engine/ -> build/mkxpz.3dsx)
E=$BUILD/engine
mkdir -p "$E/romfs/fonts"
# the Ruby parts, as C strings
python3 -I - "$HERE/engine" "$E/embedded.h" <<'PY'
import sys, os
src, out = sys.argv[1], sys.argv[2]
with open(out, 'w') as o:
    o.write('/* made by build.sh */\n')
    # (widescreen.rb: the PC patcher's, the same file)
    for name in ['boot.rb', 'ruby/module_rpg1.rb', 'ruby/module_rpg2.rb', 'ruby/module_rpg3.rb',
                 '../../../patcher-rs/assets/compat/widescreen.rb']:
        data = open(os.path.join(src, name), 'rb').read() + b'\0'
        var = os.path.basename(name).replace('.', '_')
        o.write('static const char %s[] = {\n' % var)
        for i in range(0, len(data), 16):
            o.write('  ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
        o.write('};\n')
PY
# the vertex shader (picasso), as a C array
picasso -o "$E/vshader.shbin" "$HERE/engine/vshader.v.pica"
python3 -I - "$E/vshader.shbin" "$E/vshader_shbin.h" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as o:
    o.write('/* made by build.sh */\nstatic const unsigned char vshader_shbin[] __attribute__((aligned(4))) = {\n')
    for i in range(0, len(data), 16):
        o.write('  ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',\n')
    o.write('};\nstatic const unsigned int vshader_shbin_size = %d;\n' % len(data))
PY
# the fallback font (mkxp-z's Liberation Sans)
cp "$(dirname "$RUBY_SRC")/../assets/liberation.ttf" "$E/romfs/fonts/" 2>/dev/null || true
OBJS=""
for f in main types bitmap font scene window graphics input gpu tilemap audio settings files memory profile rbinternal events battery pack movie menu; do
    arm-none-eabi-gcc $CF -O2 -Wall -Wno-unused-parameter -include mkxpz-3ds-compat.h -I"$E" \
        -I"$R/.ext/include/arm-eabi" -I"$RS/include" -I"$DEVKITPRO/portlibs/3ds/include" \
        -I"$DEVKITPRO/portlibs/3ds/include/freetype2" $( [ $f = rbinternal ] && echo "-I$RS -I$R" ) \
        -c "$HERE/engine/$f.c" -o "$E/$f.o"
    OBJS="$OBJS $E/$f.o"
done
arm-none-eabi-gcc -specs=3dsx.specs $ARCH -Wl,--gc-sections -Wl,--wrap=fcntl -Wl,--wrap=select -o "$E/mkxpz.elf" \
    $OBJS "$T/compat.o" "$T/pipe.o" "$R/ext/extinit.o" "$R/enc/encinit.o" \
    -Wl,--start-group "$R/libruby-static.a" $EXTS -Wl,--end-group \
    -L"$DEVKITPRO/libctru/lib" -L"$DEVKITPRO/portlibs/3ds/lib" -lfreetype -lbz2 -lpng16 -ltheoradec -lvorbisidec -logg -lmpg123 -llz4 -lz -lm -lcitro3d -lctru
# the one app for all games (the game list: engine/menu.c); mkxpz.3dsx the
# same, for tools/make-package.py (a game's own app, as before)
smdhtool --create "MKXP-Z 3DS" "RPG Maker XP/VX/VX Ace games" "MKXP-Z Patcher" \
    "$HERE/icon.png" "$E/mkxpz.smdh"
3dsxtool "$E/mkxpz.elf" "$BUILD/mkxpz.3dsx" --smdh="$E/mkxpz.smdh" --romfs="$E/romfs"
cp "$BUILD/mkxpz.3dsx" "$BUILD/MKXP-Z 3DS.3dsx"
echo "Built $BUILD/MKXP-Z 3DS.3dsx"
