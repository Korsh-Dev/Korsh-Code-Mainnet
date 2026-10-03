#!/usr/bin/env bash
# Native MSYS2/MinGW only. This is not a Wine security acceptance test.
export LC_ALL=C
set -euo pipefail
if [[ $# != 2 || ( $2 != core && $2 != privileged ) || ! -d $1 ]]; then
    printf 'Usage: bash %s EXISTING_DISPOSABLE_NTFS_PARENT core|privileged\n' "$0" >&2
    exit 2
fi
case "$(uname -s)" in MINGW*|MSYS*) ;; *) printf 'Native Windows/MSYS2 required; Wine is not equivalent.\n' >&2; exit 2 ;; esac
src=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
parent=$(cd "$1" && pwd)
# All outputs are in a unique owned directory, never the application build tree.
work=$(mktemp -d "$parent/acl-build-NONSECRET-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
mode=${ACL_QT_LINKAGE:-dynamic}
link=()
case "$mode" in
    static) link=(-static -static-libgcc -static-libstdc++); pkg=(--static) ;;
    dynamic) pkg=() ;;
    *) printf 'ACL_QT_LINKAGE must be static or dynamic\n' >&2; exit 2 ;;
esac
# Explicit static mode supplies Qt's private dependency list as well as MinGW runtimes.
qt_cflags_text=$(pkg-config --cflags Qt5Core)
qt_libs_text=$(pkg-config "${pkg[@]}" --libs Qt5Core)
read -r -a qt_cflags <<< "$qt_cflags_text"
read -r -a qt_libs <<< "$qt_libs_text"
if [[ $mode == static ]]; then
    # MSYS2 Qt 5.15.19's .pc lists extensionless exact filenames for these
    # two archives (-l:libz / -l:libzstd). GNU ld cannot resolve them.
    # Use their conventional names; -static still requires real archives.
    for i in "${!qt_libs[@]}"; do
        case "${qt_libs[$i]}" in
            -l:libz) qt_libs[i]=-lz ;;
            -l:libzstd) qt_libs[i]=-lzstd ;;
        esac
    done
fi
printf 'Native ACL suite=%s Qt linkage=%s Qt version=%s\n' "$2" "$mode" "$(pkg-config --modversion Qt5Core)"
sha256sum "$src/src/qt/masternodewizardconfig.h" "$src/src/qt/test/masternodewizard_windows_acl_test.cpp"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -fPIC -D_WIN32_WINNT=0x0A00 -DNOMINMAX \
    -I"$src/src" "${qt_cflags[@]}" "$src/src/qt/test/masternodewizard_windows_acl_test.cpp" \
    "${link[@]}" "${qt_libs[@]}" -ladvapi32 -o "$work/acl-test.exe"
sha256sum "$work/acl-test.exe"
"$work/acl-test.exe" "$(cygpath -m "$parent")" "$2"
