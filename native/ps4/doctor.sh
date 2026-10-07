#!/usr/bin/env bash
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0
ok(){ printf '[ OK ] %s\n' "$*"; }
bad(){ printf '[FAIL] %s\n' "$*"; status=1; }

vars=""
for c in "${PS4VARS:-}" "${OPENORBIS:-}/ps4vars.sh" "${OO_PS4_TOOLCHAIN:-}/ps4vars.sh" /opt/pacbrew/ps4/openorbis/ps4vars.sh; do
  if [[ -n "$c" && -f "$c" ]]; then vars="$c"; break; fi
done
if [[ -z "$vars" ]]; then bad 'OpenOrbis ps4vars.sh not found'; exit 1; fi
ok "ps4vars: $vars"
# shellcheck disable=SC1090
source "$vars"
for x in openorbis-cmake openorbis-pkg-config cmake git python3; do
  command -v "$x" >/dev/null && ok "$x: $(command -v "$x")" || bad "$x missing"
done
# PacBrew's older libcurl package can miss libcurl.pc even though the static
# archive and headers are installed. Mirror ps4/build.sh's local workaround.
if ! openorbis-pkg-config --exists libcurl 2>/dev/null; then
  oo="${OO_PS4_TOOLCHAIN:-${OPENORBIS:-/opt/pacbrew/ps4/openorbis}}"
  if [[ -f "$oo/usr/lib/libcurl.a" && -f "$oo/usr/include/curl/curl.h" ]]; then
    localpc="$ROOT/.ps4-pkgconfig"
    mkdir -p "$localpc"
    cat > "$localpc/libcurl.pc" <<EOF_PC
prefix=$oo/usr
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
Name: libcurl
Description: libcurl for OpenOrbis PS4
Version: 7.80.0
Libs: -L\${libdir} -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lSceNet
Cflags: -I\${includedir}
EOF_PC
    export PKG_CONFIG_PATH="$localpc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    ok "libcurl pkg-config shim: $localpc/libcurl.pc"
  fi
fi

for pc in sdl2 freetype2 libavformat libavcodec libavutil libswresample libswscale libcurl libwebp fribidi; do
  if openorbis-pkg-config --exists "$pc" 2>/dev/null; then
    ok "$pc $(openorbis-pkg-config --modversion "$pc" 2>/dev/null || true)"
  else
    bad "$pc missing"
    case "$pc" in
      libcurl) echo "       repair: sudo pacman --config ~/stremio-ps4-build/pacman-pacbrew.conf -S --needed ps4-openorbis-libcurl" ;;
      libwebp) echo "       repair: sudo pacman --config ~/stremio-ps4-build/pacman-pacbrew.conf -S --needed ps4-openorbis-libwebp" ;;
      fribidi) echo "       repair: sudo pacman --config ~/stremio-ps4-build/pacman-pacbrew.conf -S --needed ps4-openorbis-libfribidi" ;;
    esac
  fi
done
[[ -f "$ROOT/CMakeLists.txt" ]] && ok 'patched project CMakeLists.txt present' || bad 'run apply_ps4_port.py first'
exit "$status"
