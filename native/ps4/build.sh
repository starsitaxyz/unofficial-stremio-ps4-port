#!/usr/bin/env bash
# Stremio PS4 cross-build with PacBrew/OpenOrbis (real-hardware tested UI path).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-ps4}"
RML_SRC="${RMLUI_SRC:-$ROOT/.ps4-deps/RmlUi}"
LOG="$BUILD/build.log"

mkdir -p "$BUILD"
exec > >(tee "$LOG") 2>&1

echo "== Stremio PS4 real-hardware build =="
echo "host: $(uname -a)"
date -u '+UTC: %Y-%m-%dT%H:%M:%SZ'

# PacBrew normally installs here, but allow portable/custom installations.
find_ps4vars() {
    local c
    for c in \
        "${PS4VARS:-}" \
        "${OPENORBIS:-}/ps4vars.sh" \
        "${OO_PS4_TOOLCHAIN:-}/ps4vars.sh" \
        "${PACBREW:-}/ps4/openorbis/ps4vars.sh" \
        "/opt/pacbrew/ps4/openorbis/ps4vars.sh"; do
        [[ -n "$c" && -f "$c" ]] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

VARS="$(find_ps4vars || true)"
if [[ -z "$VARS" ]]; then
    echo "PacBrew/OpenOrbis ps4vars.sh not found." >&2
    echo "Expected /opt/pacbrew/ps4/openorbis/ps4vars.sh or set PS4VARS/OPENORBIS." >&2
    echo "Windows users: run windows/build-windows.ps1 from the overlay." >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$VARS"

: "${OPENORBIS:=${OO_PS4_TOOLCHAIN:-}}"
: "${OO_PS4_TOOLCHAIN:=${OPENORBIS:-}}"
export OPENORBIS OO_PS4_TOOLCHAIN

echo "ps4vars=$VARS"
echo "OPENORBIS=$OPENORBIS"
echo "OO_PS4_TOOLCHAIN=$OO_PS4_TOOLCHAIN"

command -v openorbis-cmake >/dev/null || { echo "openorbis-cmake not found" >&2; exit 1; }
command -v openorbis-pkg-config >/dev/null || { echo "openorbis-pkg-config not found" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }
command -v git >/dev/null || { echo "git not found" >&2; exit 1; }

if command -v clang >/dev/null; then clang --version | head -1 || true; fi

# PacBrew libcurl 7.80.0-3 can be installed without a usable libcurl.pc.
# If the archive/header are present, provide a repo-local pkg-config shim instead
# of requiring root or modifying /opt/pacbrew.
LOCAL_PC="$ROOT/.ps4-pkgconfig"
if ! openorbis-pkg-config --exists libcurl 2>/dev/null; then
    CURL_A="$OO_PS4_TOOLCHAIN/usr/lib/libcurl.a"
    CURL_H="$OO_PS4_TOOLCHAIN/usr/include/curl/curl.h"
    if [[ -f "$CURL_A" && -f "$CURL_H" ]]; then
        mkdir -p "$LOCAL_PC"
        cat > "$LOCAL_PC/libcurl.pc" <<EOF_PC
prefix=$OO_PS4_TOOLCHAIN/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libcurl
Description: libcurl for OpenOrbis PS4
Version: 7.80.0
Libs: -L\${libdir} -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lSceNet
Cflags: -I\${includedir}
EOF_PC
        export PKG_CONFIG_PATH="$LOCAL_PC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        echo "Using local libcurl.pc compatibility shim: $LOCAL_PC/libcurl.pc"
    fi
fi

# Fail before CMake with the exact missing PS4 library.
required_pc=(sdl2 freetype2 libavformat libavcodec libavutil libswresample libswscale libcurl libwebp fribidi)
missing=()
for pc in "${required_pc[@]}"; do
    if ! openorbis-pkg-config --exists "$pc"; then
        missing+=("$pc")
        printf '%-14s %s\n' "$pc" MISSING
    else
        printf '%-14s %s\n' "$pc" "$(openorbis-pkg-config --modversion "$pc" 2>/dev/null || echo present)"
    fi
done
if ((${#missing[@]})); then
    echo >&2
    echo "Missing PS4 portlibs: ${missing[*]}" >&2
    echo "Install/update PacBrew: sudo pacman -S --needed ps4-openorbis ps4-openorbis-portlibs" >&2
    exit 2
fi

mkdir -p "$ROOT/.ps4-deps"
if [[ ! -f "$RML_SRC/CMakeLists.txt" ]]; then
    echo "Fetching RmlUi 6.2..."
    rm -rf "$RML_SRC"
    git clone --depth 1 --branch 6.2 https://github.com/mikke89/RmlUi.git "$RML_SRC"
fi

# OpenOrbis' C++ math headers expose several float calls through their double
# variants. RmlUi 6.2 then hits C++11 narrowing errors in Math.cpp. Patch only
# these known expressions, idempotently, before CMake builds RmlUi.
RML_MATH="$RML_SRC/Source/Core/Math.cpp"
if [[ -f "$RML_MATH" ]]; then
    python3 - "$RML_MATH" <<'PY_RML'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text(encoding="utf-8")
repls = {
    'return std::modf(value, integral);': 'return ::modff(value, integral);',
    'position = Vector2f(std::floor(position.x), std::floor(position.y));':
        'position = Vector2f(static_cast<float>(std::floor(position.x)), static_cast<float>(std::floor(position.y)));',
    'size = Vector2f(std::ceil(bottom_right.x), std::ceil(bottom_right.y)) - position;':
        'size = Vector2f(static_cast<float>(std::ceil(bottom_right.x)), static_cast<float>(std::ceil(bottom_right.y))) - position;',
    'const Vector2f top_left = {std::floor(rectangle.Left()), std::floor(rectangle.Top())};':
        'const Vector2f top_left = {static_cast<float>(std::floor(rectangle.Left())), static_cast<float>(std::floor(rectangle.Top()))};',
    'const Vector2f bottom_right = {std::ceil(rectangle.Right()), std::ceil(rectangle.Bottom())};':
        'const Vector2f bottom_right = {static_cast<float>(std::ceil(rectangle.Right())), static_cast<float>(std::ceil(rectangle.Bottom()))};',
    'return std::abs(value);': 'return ::fabsf(value);',
    'return {std::abs(value.x), std::abs(value.y)};': 'return {::fabsf(value.x), ::fabsf(value.y)};',
    'return std::cos(angle);': 'return ::cosf(angle);',
    'return std::acos(value);': 'return ::acosf(value);',
    'return std::sin(angle);': 'return ::sinf(angle);',
    'return std::asin(value);': 'return ::asinf(value);',
    'return std::tan(angle);': 'return ::tanf(angle);',
    'return std::atan2(y, x);': 'return ::atan2f(y, x);',
    'return std::exp(value);': 'return ::expf(value);',
    'float result = std::fmod(angle, RMLUI_PI * 2.0f);': 'float result = ::fmodf(angle, RMLUI_PI * 2.0f);',
    'return std::sqrt(value);': 'return ::sqrtf(value);',
    'return std::floor(value + 0.5f);': 'return ::floorf(value + 0.5f);',
    'return std::ceil(value);': 'return ::ceilf(value);',
    'return std::floor(value);': 'return ::floorf(value);',
    'return int(std::ceil(value));': 'return int(::ceilf(value));',
    'return int(std::floor(value));': 'return int(::floorf(value));',
}
changed = False
for old, new in repls.items():
    if old in s:
        s = s.replace(old, new)
        changed = True
if changed:
    p.write_text(s, encoding="utf-8")
    print("Patched RmlUi 6.2 Math.cpp for OpenOrbis float math")
else:
    print("RmlUi Math.cpp float patch already applied (or upstream changed)")
PY_RML
fi

# Avoid accidentally reusing a desktop/other-toolchain cache.
if [[ -f "$BUILD/CMakeCache.txt" ]] && ! grep -q 'PS4_NATIVE:BOOL=ON' "$BUILD/CMakeCache.txt"; then
    echo "Removing incompatible CMake cache from $BUILD"
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
fi

openorbis-cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPS4_NATIVE=ON \
    -DRMLUI_PS4_SOURCE="$RML_SRC"

# PacBrew SDL2 2.0.18 can leak clang's driver-only -pthread switch into the
# final link command. OpenOrbis invokes ld.lld directly, where -pthread is not
# a valid argument. Keep the real libpthread.a link (-lpthread) and strip only
# the invalid driver switch from the generated command. This exact workaround
# was validated on the real-hardware build.
LINK_TXT="$BUILD/CMakeFiles/stremio.dir/link.txt"
if [[ -f "$LINK_TXT" ]]; then
    sed -i -E 's/(^|[[:space:]])-pthread([[:space:]]|$)/ /g' "$LINK_TXT"
fi

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
echo "Building with $JOBS jobs..."
cmake --build "$BUILD" -j"$JOBS" --target ps4_pkg

echo
printf '%s\n' "== PS4 output =="
mapfile -t outputs < <(find "$BUILD" -maxdepth 3 -type f \( -name '*.pkg' -o -name 'eboot.bin' -o -name '*.self' -o -name '*.oelf' \) -print | sort)
printf '%s\n' "${outputs[@]}"

if command -v sha256sum >/dev/null; then
    : > "$BUILD/SHA256SUMS"
    for f in "${outputs[@]}"; do sha256sum "$f" >> "$BUILD/SHA256SUMS"; done
    cat "$BUILD/SHA256SUMS"
fi
