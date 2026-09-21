#!/usr/bin/env bash
# Compare build variants of the miner on this machine (no node needed). Every variant must print the same
# selftest digest: that proves it computes exactly the consensus hash. Stop other miners first.
#
#   contrib/korsh-miner/bench.sh [--threads N] [--seconds S]
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
yp="$here/../../src/crypto/yespower"
THREADS="$(( $(nproc 2>/dev/null || echo 2) - 2 ))" SECS=10
while [ $# -gt 0 ]; do
    case "$1" in
        --threads) THREADS="${2:?}"; shift ;;
        --seconds) SECS="${2:?}"; shift ;;
        -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
LIBS="-lcurl -ljansson -lcrypto -lm"

build() {   # build <name> <sed script for yespower-platform.c or ""> <cflags...>
    local name="$1" patch="$2"; shift 2
    cp -r "$yp" "$work/y_$name"
    [ -z "$patch" ] || sed -i "$patch" "$work/y_$name/yespower-platform.c"
    # shellcheck disable=SC2086
    gcc "$@" -pthread "-I$work/y_$name" "-I$here" -o "$work/m_$name" "$here/korsh-miner.c" "$here/korsh-yp2.c" "$work/y_$name/yespower.c" $LIBS 2>/dev/null \
        || echo "  (variant $name does not build with this compiler/CPU)"
}
run() {
    [ -x "$work/m_$1" ] || return
    local w
    for w in 1 2; do   # 1 = reference kernel, 2 = two-way interleaved kernel
        printf '%-16s' "$1 ways=$w"
        "$work/m_$1" --bench "$SECS" --threads "$THREADS" --ways "$w" 2>&1 | sed -n '1p;3p' | sed -E 's/threads=[0-9]+ (ways=[0-9]+ )?seconds=[0-9.]+ //; s/selftest digest: /digest=/' | tr '\n' ' '
        echo
    done
}

echo "threads=$THREADS, ${SECS}s per variant; reserved 2 MB huge pages: $(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo ?)"
build native      ""  -O3 -march=native
build sse2        ""  -O3 -march=x86-64 -msse2
build avx2        ""  -O3 -march=x86-64-v3
build hugepages   's/(12 \* 1024 \* 1024)/(1)/' -O3 -march=native
for v in native sse2 avx2 hugepages; do run "$v"; done
echo "All digests must be identical. Speed differences under about 2% are measurement noise."
