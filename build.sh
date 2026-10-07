#!/usr/bin/env bash
# One-command Stremio PS4 builder.
# Tested on CachyOS/Arch x86_64. Ubuntu/Debian x86_64 is also supported.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${STREMIO_PS4_WORK:-$ROOT/.build}"
SRC="$WORK/src"
DIST="${STREMIO_PS4_DIST:-$ROOT/dist}"
LOG="$DIST/build.log"
UPSTREAM_REF="${STREMIO_UPSTREAM_REF:-89c0e6227cfb6549cc6e144a0a77fa852b8e316c}"
PACCONF="$WORK/pacman-pacbrew.conf"

if [[ "${1:-}" == "clean" ]]; then
    rm -rf "$WORK" "$DIST"
    echo "Cleaned $WORK and $DIST"
    exit 0
fi

mkdir -p "$WORK" "$DIST"
exec > >(tee "$LOG") 2>&1

finish() {
    rc=$?
    set +e
    if [[ -f "$SRC/build-ps4/build.log" ]]; then
        cp -f "$SRC/build-ps4/build.log" "$DIST/compiler.log" 2>/dev/null || true
    fi
    if [[ $rc -ne 0 ]]; then
        echo
        echo "Build failed with code $rc"
        echo "Logs:"
        echo "  $LOG"
        [[ -f "$DIST/compiler.log" ]] && echo "  $DIST/compiler.log"
    fi
    exit "$rc"
}
trap finish EXIT

if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "OpenOrbis/PacBrew requires an x86_64 Linux host." >&2
    exit 1
fi

if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=""
else
    command -v sudo >/dev/null 2>&1 || { echo "sudo is required." >&2; exit 1; }
    SUDO="sudo"
fi

[[ -r /etc/os-release ]] || { echo "Cannot detect Linux distribution." >&2; exit 1; }
# shellcheck disable=SC1091
. /etc/os-release

echo "== Stremio PS4 build =="
echo "Host: ${PRETTY_NAME:-${ID:-Linux}} / $(uname -m)"
echo "Upstream: Sp9nky/unofficial-stremio-ps5-port@$UPSTREAM_REF"
echo "Output: $DIST"

auto_pacbrew_config() {
    mkdir -p "$(dirname "$PACCONF")"
    if [[ -f /etc/pacman.conf ]]; then
        cp /etc/pacman.conf "$PACCONF"
    else
        cat > "$PACCONF" <<'PACBASE'
[options]
Architecture = auto
SigLevel = Required DatabaseOptional
LocalFileSigLevel = Optional
PACBASE
    fi
    if ! grep -q '^\[pacbrew\]' "$PACCONF"; then
        cat >> "$PACCONF" <<'PACBREW'

[pacbrew]
SigLevel = Optional TrustAll
Server = https://pacman.mydedibox.fr/pacbrew/packages/
PACBREW
    fi
}

install_host_and_sdk() {
    case "${ID:-}" in
        arch|cachyos|endeavouros|manjaro)
            echo "== Host dependencies (Arch/CachyOS) =="
            $SUDO pacman -S --needed --noconfirm \
                base-devel cmake ninja nasm git curl ca-certificates python pkgconf \
                zip unzip xz file openssl autoconf automake libtool libxml2-legacy
            auto_pacbrew_config
            echo "== OpenOrbis + PacBrew portlibs =="
            $SUDO pacman --config "$PACCONF" -Sy --noconfirm
            $SUDO pacman --config "$PACCONF" -S --needed --noconfirm \
                ps4-openorbis ps4-openorbis-portlibs \
                ps4-openorbis-sdl2 ps4-openorbis-freetype ps4-openorbis-ffmpeg \
                ps4-openorbis-libcurl ps4-openorbis-libwebp ps4-openorbis-libfribidi
            ;;
        ubuntu|debian)
            echo "== Host dependencies (Ubuntu/Debian) =="
            $SUDO apt-get update
            $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
                pacman-package-manager makepkg libarchive-tools build-essential \
                autoconf automake libtool cmake ninja-build nasm git curl ca-certificates \
                python3 python3-setuptools pkg-config zip unzip xz-utils file \
                openssl libssl-dev libxml2
            auto_pacbrew_config
            echo "== OpenOrbis + PacBrew portlibs =="
            $SUDO pacman --config "$PACCONF" -Sy --noconfirm
            $SUDO pacman --config "$PACCONF" -S --needed --noconfirm \
                ps4-openorbis ps4-openorbis-portlibs \
                ps4-openorbis-sdl2 ps4-openorbis-freetype ps4-openorbis-ffmpeg \
                ps4-openorbis-libcurl ps4-openorbis-libwebp ps4-openorbis-libfribidi
            ;;
        *)
            echo "Unsupported distro: ${PRETTY_NAME:-${ID:-unknown}}" >&2
            echo "Tested: CachyOS/Arch. Supported by this script: Arch-family, Ubuntu, Debian." >&2
            exit 1
            ;;
    esac
}

install_host_and_sdk

VARS=/opt/pacbrew/ps4/openorbis/ps4vars.sh
[[ -f "$VARS" ]] || { echo "OpenOrbis install incomplete: missing $VARS" >&2; exit 2; }

# Always recreate the patched source tree. The original project is small enough
# that this keeps builds deterministic and avoids stale patch state.
echo "== Preparing source =="
rm -rf "$SRC"
git clone --no-tags https://github.com/Sp9nky/unofficial-stremio-ps5-port.git "$SRC"
git -C "$SRC" checkout --detach "$UPSTREAM_REF"
python3 "$ROOT/native/apply_ps4_port.py" "$SRC"
chmod +x "$SRC"/ps4/*.sh 2>/dev/null || true

# Useful for build logs and reproducibility.
echo "$UPSTREAM_REF" > "$DIST/UPSTREAM_COMMIT"

echo "== Checking PS4 dependencies =="
cd "$SRC"
bash ps4/doctor.sh

echo "== Building PKG =="
CI=1 bash ps4/build.sh

echo "== Collecting output =="
rm -f "$DIST"/*.pkg "$DIST"/eboot.bin "$DIST"/SHA256SUMS 2>/dev/null || true
find "$SRC/build-ps4" -maxdepth 4 -type f -name 'Stremio-PS4-*.pkg' -exec cp -fv {} "$DIST/" \;
if [[ -f "$SRC/build-ps4/eboot.bin" ]]; then
    cp -fv "$SRC/build-ps4/eboot.bin" "$DIST/eboot.bin"
fi
if [[ -f "$SRC/build-ps4/build.log" ]]; then
    cp -f "$SRC/build-ps4/build.log" "$DIST/compiler.log"
fi

mapfile -t PKGS < <(find "$DIST" -maxdepth 1 -type f -name '*.pkg' -print | sort)
if ((${#PKGS[@]} == 0)); then
    echo "Build completed without a PKG in $DIST." >&2
    exit 3
fi

: > "$DIST/SHA256SUMS"
for f in "${PKGS[@]}"; do
    sha256sum "$f" >> "$DIST/SHA256SUMS"
done

echo
echo "BUILD OK"
ls -lh "${PKGS[@]}"
cat "$DIST/SHA256SUMS"

trap - EXIT
