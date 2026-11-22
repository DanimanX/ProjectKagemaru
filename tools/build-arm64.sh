#!/usr/bin/env bash
# Cross-compile ProjectKagemaru (ROM-free distributable runtime) for AArch64 Linux
# handhelds (Anbernic RG34XX / Allwinner H700, RG35XX Plus/H/SP, RG40XX,
# RG CubeXX, Powkiddy RGB30, TrimUI Smart Pro) targeting glibc <= 2.28 +
# OpenGL ES 2.0 + ARM64 SLJIT + NEON, and assemble a PortMaster zip package.
#
# Zero copyrighted ROMs or ROM-derived game code ever enter this package.
#
# Usage:
#   tools/build-arm64.sh                    # default CPU: cortex_a53 (H700)
#   TARGET_CPU=cortex_a55 tools/build-arm64.sh
set -euo pipefail
cd "$(dirname "$0")/.."

DEPS="$PWD/deps"
EXTERN="$PWD/extern"
BDIR="$PWD/build-arm64"
OUT="$PWD/dist/ProjectKagemaru-PortMaster-aarch64"
ZIPOUT="$PWD/dist/ProjectKagemaru-PortMaster-aarch64.zip"
TARGET_CPU="${TARGET_CPU:-cortex_a53}"
TARGET_CPU="${TARGET_CPU//-/_}"
TARGET_TRIPLE="aarch64-linux-gnu.2.28"
JOBS="${JOBS:-$(nproc)}"

ZIG_VER="0.17.0"
ZIG_SHA="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
SDL_VER="2.32.10"
SDL_SHA="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
ZLIB_VER="1.3.1"
ZLIB_SHA="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"

IMGUI_URL="https://github.com/ocornut/imgui.git"
IMGUI_REV="f1cc2ae15e53a861a874c3034aae6798fde194ab"

mkdir -p "$DEPS/dl" "$EXTERN" "$BDIR/obj" "$BDIR/rt" "$BDIR/ui" "$BDIR/zlib" "$BDIR/musashi"

fetch_file() {
    local url="$1" name="$2" sha="$3"
    if [ ! -f "$DEPS/dl/$name" ]; then
        curl -fsSL --retry 3 -o "$DEPS/dl/$name.part" "$url"
        mv "$DEPS/dl/$name.part" "$DEPS/dl/$name"
    fi
    echo "$sha  $DEPS/dl/$name" | sha256sum -c --quiet || {
        echo "ERROR: SHA-256 mismatch for $DEPS/dl/$name" >&2
        exit 1
    }
}

fetch_git() {
    local url="$1" rev="$2" dest="$3" sentinel="$4"
    if [ ! -d "$dest/.git" ]; then
        git clone --quiet --no-checkout "$url" "$dest"
    fi
    if [ ! -f "$dest/$sentinel" ] || [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)" != "$rev" ]; then
        git -C "$dest" fetch --quiet --depth 1 origin "$rev" 2>/dev/null || git -C "$dest" fetch --quiet origin || true
        git -C "$dest" checkout --quiet --force --detach "$rev"
    fi
    [ -f "$dest/$sentinel" ] || { echo "ERROR: missing $dest/$sentinel after checkout of $rev" >&2; exit 1; }
}

# 1. Host Zig compiler (hermetic aarch64-linux-gnu.2.28 C/C++20 cross-compiler)
if [ ! -x "$DEPS/zig-linux/zig" ]; then
    echo "[1/6] Fetching Zig $ZIG_VER..."
    fetch_file "https://ziglang.org/download/$ZIG_VER/zig-x86_64-linux-$ZIG_VER.tar.xz" \
        "zig-x86_64-linux-$ZIG_VER.tar.xz" "$ZIG_SHA"
    tar -C "$DEPS" -xJf "$DEPS/dl/zig-x86_64-linux-$ZIG_VER.tar.xz"
    rm -rf "$DEPS/zig-linux"
    mv "$DEPS/zig-x86_64-linux-$ZIG_VER" "$DEPS/zig-linux"
fi
ZIG_BIN="$DEPS/zig-linux/zig"
export ZIG_GLOBAL_CACHE_DIR="$DEPS/zig-cache"
export ZIG_LOCAL_CACHE_DIR="$DEPS/zig-cache"

# 2. Pinned upstream third-party dependencies (Dear ImGui, zlib 1.3.1)
echo "[2/6] Checking pinned dependencies (Dear ImGui, zlib $ZLIB_VER)..."
fetch_git "$IMGUI_URL" "$IMGUI_REV" "$EXTERN/imgui" "imgui.cpp"
if [ ! -f "$DEPS/zlib-src/zlib-$ZLIB_VER/zlib.h" ]; then
    fetch_file "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" \
        "zlib-$ZLIB_VER.tar.gz" "$ZLIB_SHA"
    rm -rf "$DEPS/zlib-src" && mkdir -p "$DEPS/zlib-src"
    tar -C "$DEPS/zlib-src" -xzf "$DEPS/dl/zlib-$ZLIB_VER.tar.gz"
fi
ZLIB_DIR="$DEPS/zlib-src/zlib-$ZLIB_VER"

# 3. Clean SDL2 2.32.10 headers (target-independent Linux LP64 + GLES 2.0)
if [ ! -d "$DEPS/sdl2-src/SDL2-$SDL_VER/include" ]; then
    echo "[3/6] Fetching SDL2 $SDL_VER headers..."
    fetch_file "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" \
        "SDL2-$SDL_VER.tar.gz" "$SDL_SHA"
    rm -rf "$DEPS/sdl2-src" && mkdir -p "$DEPS/sdl2-src"
    tar -C "$DEPS/sdl2-src" -xzf "$DEPS/dl/SDL2-$SDL_VER.tar.gz"
fi
SDL_INC="$BDIR/inc"
rm -rf "$SDL_INC" && mkdir -p "$SDL_INC/SDL2"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/include"/*.h "$SDL_INC/SDL2/"
cat > "$SDL_INC/SDL2/SDL_config.h" <<'EOF'
#ifndef SDL_config_h_
#define SDL_config_h_
#include "SDL_platform.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#define SDL_BYTEORDER 1234
#define SIZEOF_VOIDP 8
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_MATH_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_ALLOCA_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_VSNPRINTF 1
#define HAVE_SNPRINTF 1
#define SDL_VIDEO_OPENGL_ES2 1
#define SDL_VIDEO_OPENGL_EGL 1
#endif
EOF

# 4. Cross-compile zlib, Musashi, Z80, SLJIT, MAME cores, Model 1 runtime, Dear ImGui, and ARM64 GLES 2.0 frontend
echo "[4/6] Cross-compiling ProjectKagemaru for $TARGET_TRIPLE ($TARGET_CPU, GLES 2.0, SLJIT ARM64, NEON, -j$JOBS)..."

# 4a. Static zlib
ZLIB_SRCS=(
    adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c
    infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c
)
if [ ! -f "$BDIR/libz.a" ]; then
    printf '%s\n' "${ZLIB_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
        src="'"$ZLIB_DIR"'/$1"
        out="$2/$(basename "$1" .c).o"
        [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
        exec "$3" cc -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -O2 -w -DZ_HAVE_UNISTD_H -c "$src" -o "$out"
    ' _ {} "$BDIR/zlib" "$ZIG_BIN"
    "$ZIG_BIN" ar rcs "$BDIR/libz.a" "$BDIR/zlib"/*.o
fi

# 4b. Musashi m68kmake (host tool) + C/ASM cores (SLJIT, Musashi, Z80, coro_arm64)
if [ ! -x "$BDIR/m68kmake" ]; then
    "$ZIG_BIN" cc -O1 -w third_party/musashi/m68kmake.c -o "$BDIR/m68kmake"
fi
if [ ! -f "$BDIR/musashi/m68kops.c" ]; then
    "$BDIR/m68kmake" "$BDIR/musashi" third_party/musashi/m68k_in.c >/dev/null
fi

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O3 -w -DSLJIT_CONFIG_AUTO=1 \
    -c third_party/sljit/sljitLir.c -o "$BDIR/sljitLir.o"

MUSASHI_FLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -O3 -w -Ithird_party/musashi -I$BDIR/musashi"
"$ZIG_BIN" cc $MUSASHI_FLAGS -c "$BDIR/musashi/m68kops.c" -o "$BDIR/rt/musashi_m68kops.o"
"$ZIG_BIN" cc $MUSASHI_FLAGS -c third_party/musashi/m68kcpu.c -o "$BDIR/rt/musashi_m68kcpu.o"
"$ZIG_BIN" cc $MUSASHI_FLAGS -c third_party/musashi/m68kdasm.c -o "$BDIR/rt/musashi_m68kdasm.o"
"$ZIG_BIN" cc $MUSASHI_FLAGS -c third_party/musashi/softfloat/softfloat.c -o "$BDIR/rt/musashi_softfloat.o"
"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O3 -w -c third_party/z80/z80.c -o "$BDIR/rt/z80.o"
"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -c src/rt/coro_arm64.S -o "$BDIR/rt/coro_arm64.o"

CXX_FLAGS="-target $TARGET_TRIPLE -mcpu=$TARGET_CPU -std=c++20 -O3 -fno-fast-math -fno-math-errno -ffp-contract=off \
  -DVF_NATIVE \
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function -Wno-attributes \
  -Ithird_party/glm -Isrc/core/shim_include -Isrc -Iplatform/arm64 -I$ZLIB_DIR -I$EXTERN/imgui -I$SDL_INC -I$SDL_INC/SDL2 -I$BDIR/musashi"

# 4c. Core + Machine + Runtime + Audio + Host (C++20)
RT_SRCS=(
    src/core/v60_device.cpp src/core/mb86233.cpp
    third_party/mame/v60/v60d.cpp
    src/rt/rec_rt.cpp src/rt/coro.cpp src/rt/analyse.cpp src/rt/native.cpp src/rt/rec_table_none.cpp
    src/machine/model1.cpp src/machine/model1_tgp.cpp src/machine/model1_tgp_lle.cpp
    src/machine/model1_video.cpp src/machine/model1_sky.cpp src/machine/model1_interp.cpp
    src/machine/model1_gpu.cpp src/machine/model1_edge_aa.cpp src/machine/model1io.cpp src/machine/model1_state.cpp
    src/machine/m1audio.cpp src/machine/multipcm.cpp
    third_party/ymfm/ymfm_opn.cpp third_party/ymfm/ymfm_adpcm.cpp third_party/ymfm/ymfm_ssg.cpp
    src/host/romset.cpp src/host/stb_image_impl.cpp
)

printf '%s\n' "${RT_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    tag=$(echo "$src" | tr "/." "__")
    out="$2/${tag}.o"
    extra=""
    case "$src" in
        third_party/*|src/host/stb_image_impl.cpp) extra="-w -O3 -fno-math-errno" ;;
        *) extra="-O3 -fno-math-errno" ;;
    esac
    exec "$3" c++ '"$CXX_FLAGS"' $extra -c "$src" -o "$out"
' _ {} "$BDIR/rt" "$ZIG_BIN"
rm -f "$BDIR/libruntime.a"
"$ZIG_BIN" ar rcs "$BDIR/libruntime.a" "$BDIR/rt"/*.o "$BDIR/sljitLir.o"

# 4d. Dear ImGui
UI_SRCS=(
    extern/imgui/imgui.cpp extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp extern/imgui/imgui_widgets.cpp
)
printf '%s\n' "${UI_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    [ -f "$out" ] && [ "$out" -nt "$src" ] && exit 0
    exec "$3" c++ -target '"$TARGET_TRIPLE"' -mcpu='"$TARGET_CPU"' -std=c++20 -O2 -w \
        -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -I'"$EXTERN/imgui"' -c "$src" -o "$out"
' _ {} "$BDIR/ui" "$ZIG_BIN"
rm -f "$BDIR/libimgui.a"
"$ZIG_BIN" ar rcs "$BDIR/libimgui.a" "$BDIR/ui"/*.o

# 4e. ARM64 Frontend
APP_SRCS=(platform/arm64/gpu_gles.cpp platform/arm64/main_arm64.cpp)
printf '%s\n' "${APP_SRCS[@]}" | xargs -P "$JOBS" -I{} sh -c '
    src="$1"
    out="$2/$(basename "$src" .cpp).o"
    exec "$3" c++ '"$CXX_FLAGS"' -O3 -fno-math-errno -ffp-contract=fast -c "$src" -o "$out"
' _ {} "$BDIR/obj" "$ZIG_BIN"

# 5. Link vf1.aarch64 against link-time SDL2 stub + static libz.a + static libimgui.a
echo "[5/6] Generating SDL2 link stub and linking vf1.aarch64..."
python3 - "$BDIR/sdl2_stub.c" "$BDIR/obj"/*.o <<'PYEOF'
import subprocess, sys
out_c = sys.argv[1]
objs = sys.argv[2:]
syms = set()
for o in objs:
    p = subprocess.run(["nm", "-u", o], capture_output=True, text=True, check=True)
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == "U" and parts[-1].startswith("SDL_"):
            syms.add(parts[-1])
with open(out_c, "w") as f:
    f.write("/* Link-time-only stub for PortMaster system libSDL2-2.0.so.0 */\n")
    for s in sorted(syms):
        f.write(f"void {s}(void) {{}}\n")
print(f"  Generated SDL2 stub with {len(syms)} symbols")
PYEOF

"$ZIG_BIN" cc -target "$TARGET_TRIPLE" -shared -Wl,-soname,libSDL2-2.0.so.0 \
    -o "$BDIR/libSDL2-2.0.so.0" "$BDIR/sdl2_stub.c"

"$ZIG_BIN" c++ -target "$TARGET_TRIPLE" -mcpu="$TARGET_CPU" -O2 -fno-fast-math -ffp-contract=off \
    -o "$BDIR/vf1.aarch64" \
    "$BDIR/obj"/*.o \
    "$BDIR/libruntime.a" "$BDIR/libimgui.a" "$BDIR/libz.a" \
    -L"$BDIR" -l:libSDL2-2.0.so.0 -lm -ldl -lpthread

# 6. Verify ELF architecture, dynamic deps, glibc <= 2.28
echo "[6/6] Verifying AArch64 binary and assembling ROM-free PortMaster package..."
FILE_INFO=$(file "$BDIR/vf1.aarch64")
echo "$FILE_INFO" | grep -q "ARM aarch64" || { echo "ERROR: not ARM aarch64 ($FILE_INFO)" >&2; exit 1; }
MAX_GLIBC=$(objdump -T "$BDIR/vf1.aarch64" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [ -n "$MAX_GLIBC" ] && [ "$(printf '%s\n2.28\n' "$MAX_GLIBC" | sort -V | tail -1)" != "2.28" ]; then
    echo "ERROR: vf1.aarch64 requires GLIBC_$MAX_GLIBC (max allowed 2.28)" >&2
    exit 1
fi
READELF_D=$(readelf -d "$BDIR/vf1.aarch64")
if printf '%s\n' "$READELF_D" | grep -qE "libstdc\+\+|libz\.so|libGLESv2"; then
    echo "ERROR: unexpected libstdc++.so, libz.so, or libGLESv2.so dependency" >&2
    exit 1
fi
echo "  OK: vf1.aarch64 -> ARM aarch64, max GLIBC_${MAX_GLIBC:-none}, zero libstdc++.so/libz.so/libGLESv2.so"
readelf -d "$BDIR/vf1.aarch64" | grep NEEDED

# Assemble ROM-free PortMaster directory & zip
rm -rf "$OUT" "$ZIPOUT"
mkdir -p "$OUT/projectkagemaru/roms" "$OUT/projectkagemaru/saves" \
         "$OUT/projectkagemaru/libs.aarch64" "$OUT/projectkagemaru/licenses" \
         "$OUT/projectkagemaru/assets/backgrounds"

cp portmaster/*.sh "$OUT/"
chmod +x "$OUT/"*.sh
cp portmaster/projectkagemaru/port.json "$OUT/projectkagemaru/"
cp portmaster/projectkagemaru/gameinfo.xml "$OUT/projectkagemaru/"
cp portmaster/projectkagemaru/gamecontrollerdb.txt "$OUT/projectkagemaru/"
cp portmaster/projectkagemaru/PUT-ROMS-HERE.txt "$OUT/projectkagemaru/roms/PUT-ROMS-HERE.txt"
cp assets/vf_map.txt "$OUT/projectkagemaru/assets/vf_map.txt"
[ -f assets/projectkagemaru-banner.jpg ] && cp assets/projectkagemaru-banner.jpg "$OUT/projectkagemaru/cover.jpg"
cp "$BDIR/vf1.aarch64" "$OUT/projectkagemaru/vf1.aarch64"
chmod +x "$OUT/projectkagemaru/vf1.aarch64"

cp LICENSE "$OUT/projectkagemaru/licenses/ProjectKagemaru-MIT.txt"
cp third_party/mame/README.md "$OUT/projectkagemaru/licenses/MAME-BSD3-README.md"
cp third_party/sljit/LICENSE "$OUT/projectkagemaru/licenses/SLJIT-BSD2.txt"
cp third_party/ymfm/LICENSE "$OUT/projectkagemaru/licenses/ymfm-BSD3.txt"
cp third_party/z80/LICENSE "$OUT/projectkagemaru/licenses/Z80-MIT.txt"
cp third_party/glm/copying.txt "$OUT/projectkagemaru/licenses/GLM-MIT.txt"
cp "$DEPS/sdl2-src/SDL2-$SDL_VER/LICENSE.txt" "$OUT/projectkagemaru/licenses/SDL2-LICENSE.txt"
[ -f "$EXTERN/imgui/LICENSE.txt" ] && cp "$EXTERN/imgui/LICENSE.txt" "$OUT/projectkagemaru/licenses/DearImGui-LICENSE.txt"
[ -f "$ZLIB_DIR/LICENSE" ] && cp "$ZLIB_DIR/LICENSE" "$OUT/projectkagemaru/licenses/zlib-LICENSE.txt"
cp README.md "$OUT/projectkagemaru/README.md"

if command -v zip >/dev/null 2>&1; then
    (cd "$OUT" && zip -qr9 "$ZIPOUT" *.sh projectkagemaru)
else
    python3 -c "import shutil; shutil.make_archive('${ZIPOUT%.zip}', 'zip', '$OUT')"
fi
echo "Packaged: $ZIPOUT ($(du -h "$ZIPOUT" | cut -f1))"
