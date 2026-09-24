#!/usr/bin/env bash
# Build the miner: korsh-miner (native, recommended) and libyp.so (used by korsh_miner.py and the reference pool).
# Needs: gcc, libcurl-devel, jansson-devel, openssl-devel.
#
# Environment options:
#   KORSH_CFLAGS      compiler flags (default: "-O3 -march=native"; use "-O3 -march=x86-64-v2" for a portable binary)
#   KORSH_HUGEPAGES=1 build korsh-miner so that its Yespower memory is allocated with 2 MB huge pages when the system
#                     has some reserved (see setup-hugepages.sh). The node's yespower sources are NOT modified: the
#                     change is applied to a private temporary copy, and it only affects where memory comes from, never
#                     the hash results (check with: korsh-miner --selftest).
#
# korsh-miner includes an optional two-way interleaved Yespower kernel (korsh-yp2.c). If it cannot be compiled on your
# system the miner is built without it and uses the reference kernel only.
# shellcheck disable=SC2086  # CFLAGS and LIBS are intentionally word-split
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
yp="$here/../../src/crypto/yespower"
CF="${KORSH_CFLAGS:--O3 -march=native}"
LIBS="-lcurl -ljansson -lcrypto -lm"

# Windows (mingw/msys2): the miner talks raw sockets, so it needs the Winsock import library.
case "$(gcc -dumpmachine 2>/dev/null || echo unknown)" in
    *mingw*|*windows*|*cygwin*) LIBS="$LIBS -lws2_32" ;;
esac

if gcc $CF -fopenmp -shared -fPIC -I"$yp" -o "$here/libyp.so" "$here/scan.c" "$yp/yespower.c" 2>/dev/null; then
    echo "built $here/libyp.so"
else
    echo "note: libyp.so not built (OpenMP runtime unavailable) - the native korsh-miner build continues"
fi

ypb="$yp"
if [ "${KORSH_HUGEPAGES:-0}" = 1 ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cp -r "$yp"/. "$tmp"/
    # the upstream allocator only asks for huge pages above 12 MB; Korsh's regions are about 256 KB
    sed -i 's/(12 \* 1024 \* 1024)/(1)/' "$tmp/yespower-platform.c"
    grep -q 'define HUGEPAGE_THRESHOLD[[:space:]]*(1)' "$tmp/yespower-platform.c" ||
        { echo "error: could not patch the huge page threshold (upstream file changed?)" >&2; exit 1; }
    ypb="$tmp"
    echo "huge pages enabled in this build"
fi

log=/tmp/korsh-miner-build.log
if gcc $CF -pthread -I"$ypb" -I"$here" -o "$here/korsh-miner" "$here/korsh-miner.c" "$here/korsh-yp2.c" "$ypb/yespower.c" \
    $LIBS 2>"$log"; then
    echo "built $here/korsh-miner (with the two-way kernel)"
elif gcc $CF -pthread -DKORSH_NO_YP2 -I"$ypb" -o "$here/korsh-miner" "$here/korsh-miner.c" "$ypb/yespower.c" $LIBS 2>>"$log"; then
    echo "built $here/korsh-miner (reference kernel only: the two-way kernel did not compile, see $log)"
else
    echo "native miner not built (install libcurl, jansson and OpenSSL dev packages); see $log"
fi
