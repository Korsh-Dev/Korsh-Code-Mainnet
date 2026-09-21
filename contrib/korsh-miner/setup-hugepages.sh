#!/usr/bin/env bash
# Reserve 2 MB huge pages for a miner built with KORSH_HUGEPAGES=1 (needs root; uses sudo when available).
#
#   contrib/korsh-miner/setup-hugepages.sh [--threads N] [--persist] [--off]
#
# Each mining thread needs one 2 MB huge page for its Yespower memory; a few extra pages are reserved as slack.
# Without --persist the setting lasts until reboot. --off releases the reservation.
set -euo pipefail
THREADS="$(( $(nproc 2>/dev/null || echo 2) - 2 ))" PERSIST=no OFF=no
while [ $# -gt 0 ]; do
    case "$1" in
        --threads) THREADS="${2:?--threads needs a number}"; shift ;;
        --persist) PERSIST=yes ;;
        --off) OFF=yes ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
SUDO=""; [ "$(id -u)" -eq 0 ] || SUDO="sudo"
if [ "$OFF" = yes ]; then PAGES=0; else PAGES=$(( THREADS + 8 )); fi

$SUDO sysctl -w "vm.nr_hugepages=$PAGES"
if [ "$PERSIST" = yes ] && [ "$OFF" = no ]; then
    echo "vm.nr_hugepages=$PAGES" | $SUDO tee /etc/sysctl.d/90-korsh-hugepages.conf >/dev/null
    echo "persisted in /etc/sysctl.d/90-korsh-hugepages.conf"
elif [ "$OFF" = yes ]; then
    $SUDO rm -f /etc/sysctl.d/90-korsh-hugepages.conf
fi
grep -E 'HugePages_(Total|Free)|Hugepagesize' /proc/meminfo
echo "Reserved pages cannot be used by anything else: $((PAGES * 2)) MB are now set aside."
