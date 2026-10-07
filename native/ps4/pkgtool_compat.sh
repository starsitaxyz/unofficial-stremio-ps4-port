#!/usr/bin/env bash
# Run OpenOrbis' legacy PkgTool.Core on both OpenSSL 1.1 and OpenSSL 3 hosts.
# No system files are modified. If the bundled .NET runtime asks for 1.1,
# tiny ABI-compatibility DSOs are built in the build tree and used only here.
set -euo pipefail

[[ $# -ge 2 ]] || { echo "usage: $0 PKGTOOL WORKDIR [args...]" >&2; exit 2; }
PKGTOOL="$1"
WORKDIR="$2"
shift 2
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
COMPAT="$WORKDIR/pkgtool-openssl11-compat"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/host/openssl11shim.c"

run_pkgtool() { "$PKGTOOL" "$@"; }
run_compat() {
    LD_LIBRARY_PATH="$COMPAT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" run_pkgtool "$@"
}

build_compat() {
    [[ -f "$COMPAT/libssl.so.1.1" && -f "$COMPAT/libcrypto.so.1.1" ]] && return 0
    command -v cc >/dev/null || return 1
    [[ -f "$SRC" ]] || return 1
    printf '#include <openssl/ssl.h>\n' | cc -x c -E - >/dev/null 2>&1 || return 1
    echo ">> PkgTool.Core: using local OpenSSL 1.1 compatibility on this OpenSSL 3 host" >&2
    mkdir -p "$COMPAT"
    local cflags=(-shared -fPIC -O2 -Wno-deprecated-declarations)
    cc "${cflags[@]}" "$SRC" -o "$COMPAT/libssl.so.1.1" \
       -Wl,-soname,libssl.so.1.1 -lssl -lcrypto
    cc "${cflags[@]}" "$SRC" -o "$COMPAT/libcrypto.so.1.1" \
       -Wl,-soname,libcrypto.so.1.1 -lssl -lcrypto
}

if [[ -f "$COMPAT/libssl.so.1.1" && -f "$COMPAT/libcrypto.so.1.1" ]]; then
    run_compat "$@"
    exit $?
fi

# Avoid the .NET runtime abort on common modern Linux hosts when possible.
if command -v ldconfig >/dev/null 2>&1; then
    if ! ldconfig -p 2>/dev/null | grep -q 'libssl\.so\.1\.1' && \
       ldconfig -p 2>/dev/null | grep -q 'libssl\.so\.3'; then
        if build_compat; then
            run_compat "$@"
            exit $?
        fi
    fi
fi

# Fallback for unusual hosts where ldconfig cannot tell us. Probe once and
# only build the shim when the failure is specifically the legacy SSL ABI.
TMP="$WORKDIR/.pkgtool-probe.$$.log"
set +e
run_pkgtool "$@" > >(tee "$TMP") 2> >(tee -a "$TMP" >&2)
RC=$?
set -e
if [[ $RC -eq 0 ]]; then
    rm -f "$TMP"
    exit 0
fi
if grep -Eqi 'No usable version of libssl|libssl\.so\.1\.1|libcrypto\.so\.1\.1' "$TMP" && build_compat; then
    rm -f "$TMP"
    run_compat "$@"
    exit $?
fi
rm -f "$TMP"
exit "$RC"
