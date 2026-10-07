#!/usr/bin/env bash
set -euo pipefail

EBOOT="$1"
OUTDIR="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"
PKGTOOL_RUN="$ROOT/ps4/pkgtool_compat.sh"

TITLE="Stremio PS4"
VERSION="01.05"
DISPLAY_VERSION="0.5.0"
TITLE_ID="STRM00001"
CONTENT_ID="IV0001-STRM00001_00-STRM000010105000"

[[ -f "$EBOOT" ]] || { echo "Missing eboot.bin: $EBOOT" >&2; exit 1; }
[[ -x "$TOOLS/PkgTool.Core" || -f "$TOOLS/PkgTool.Core" ]] || { echo "PkgTool.Core not found in OpenOrbis" >&2; exit 1; }

STAGE="$OUTDIR/pkg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module"
cp "$EBOOT" "$STAGE/eboot.bin"
cp -r "$ROOT/app/assets" "$STAGE/"
cp -r "$ROOT/app/fonts" "$STAGE/"
cp "$ROOT/app/ca-bundle.crt" "$STAGE/"
[[ -d "$ROOT/app/licenses" ]] && cp -r "$ROOT/app/licenses" "$STAGE/"
[[ -f "$ROOT/LICENSE" ]] && cp "$ROOT/LICENSE" "$STAGE/"
[[ -f "$ROOT/THIRD_PARTY.md" ]] && cp "$ROOT/THIRD_PARTY.md" "$STAGE/"
[[ -f "$ROOT/PS4_PORT.md" ]] && cp "$ROOT/PS4_PORT.md" "$STAGE/"
cp "$ROOT/app/sce_sys/icon0.png" "$STAGE/sce_sys/icon0.png"

# OpenOrbis/PacBrew ships these package/runtime modules somewhere below the
# toolchain tree; locate the current copy rather than hardcoding release paths.
find_oo_file() {
    local name="$1" p
    p="$(find "$OO_PS4_TOOLCHAIN" -type f -name "$name" -print -quit 2>/dev/null || true)"
    [[ -n "$p" ]] || { echo "Missing $name in OpenOrbis installation" >&2; exit 1; }
    printf '%s' "$p"
}
cp "$(find_oo_file right.sprx)" "$STAGE/sce_sys/about/right.sprx"
cp "$(find_oo_file libc.prx)" "$STAGE/sce_module/libc.prx"
cp "$(find_oo_file libSceFios2.prx)" "$STAGE/sce_module/libSceFios2.prx"

SFO="$STAGE/sce_sys/param.sfo"
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_new "$SFO"
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" APP_TYPE --type Integer --maxsize 4 --value 1
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4 --value 0
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" CATEGORY --type Utf8 --maxsize 4 --value gde
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" FORMAT --type Utf8 --maxsize 4 --value obs
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4 --value 1020
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" sfo_setentry "$SFO" VERSION --type Utf8 --maxsize 8 --value "$VERSION"

# PacBrew's create-gp4 currently accepts --path, while the official OpenOrbis
# artifact accepts --files. Support both so the package step is not tied to one
# distributor/version of the same tool.
GP4_HELP="$("$TOOLS/create-gp4" -h 2>&1 || true)"
if grep -q -- '-path' <<<"$GP4_HELP"; then
    "$TOOLS/create-gp4" -out "$STAGE/pkg.gp4" --content-id "$CONTENT_ID" --path "$STAGE"
else
    FILES="$(cd "$STAGE" && find . -type f ! -name 'pkg.gp4' -printf '%P ' | sed 's/ $//')"
    [[ -n "$FILES" ]] || { echo "No package files found in $STAGE" >&2; exit 1; }
    (cd "$STAGE" && "$TOOLS/create-gp4" -out pkg.gp4 --content-id "$CONTENT_ID" --files "$FILES")
fi
"$PKGTOOL_RUN" "$TOOLS/PkgTool.Core" "$OUTDIR" pkg_build "$STAGE/pkg.gp4" "$OUTDIR"

PKG="$OUTDIR/$CONTENT_ID.pkg"
FINAL="$OUTDIR/Stremio-PS4-$DISPLAY_VERSION.pkg"
[[ -f "$PKG" ]] || { echo "PkgTool did not create $PKG" >&2; exit 1; }
cp -f "$PKG" "$FINAL"
echo "Built $FINAL"
