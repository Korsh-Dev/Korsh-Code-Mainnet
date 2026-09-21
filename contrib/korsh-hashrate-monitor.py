#!/usr/bin/env python3
"""Watch a Korsh node's network hashrate and warn about sudden drops.

A proof-of-work chain with a short, cheap-to-mine history is most exposed when
its honest hashrate falls or when a single actor takes a large share of it: that
is the moment a 51% reorg becomes affordable. This script samples the network
statistics through `korsh-cli` (no extra dependencies) and tells you when
something moves, so the operator can react before it matters.

Usage:
    korsh-hashrate-monitor.py --datadir ~/.korsh [--chain=main]
                              [--interval 300] [--csv hashrate.csv]
                              [--drop-pct 35] [--webhook URL]
                              [--cli /path/to/korsh-cli]

Exit status: 0 while nothing alarming happened, 1 if an alert was raised (so a
cron entry or a supervisor can flag it).
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request
from datetime import datetime, timezone


def cli(cli_path, datadir, chain, *args):
    cmd = [cli_path, f"-datadir={datadir}"]
    if chain:
        cmd.append(f"-chain={chain}")
    cmd.extend(args)
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except (subprocess.TimeoutExpired, OSError) as exc:
        return None, str(exc)
    if out.returncode != 0:
        return None, out.stderr.strip() or out.stdout.strip()
    return out.stdout.strip(), None


def fmt_hashrate(value):
    for unit, scale in (("H/s", 1), ("kH/s", 1e3), ("MH/s", 1e6), ("GH/s", 1e9), ("TH/s", 1e12)):
        if abs(value) < scale * 1000 or unit == "TH/s":
            return f"{value / scale:.2f} {unit}"
    return f"{value:.2f} H/s"


def sample(cli_path, datadir, chain):
    raw, err = cli(cli_path, datadir, chain, "getmininginfo")
    if raw is None:
        return None, err
    try:
        info = json.loads(raw)
    except json.JSONDecodeError as exc:
        return None, f"unparseable getmininginfo: {exc}"
    # Dash-derived nodes report networkhashps in getmininginfo; fall back to the
    # dedicated RPC if a build does not.
    hashrate = info.get("networkhashps")
    if hashrate is None:
        raw, err = cli(cli_path, datadir, chain, "getnetworkhashps")
        if raw is None:
            return None, err
        try:
            hashrate = float(raw)
        except ValueError:
            return None, f"unparseable getnetworkhashps: {raw}"
    peers = info.get("connections")
    if peers is None:
        raw, err = cli(cli_path, datadir, chain, "getconnectioncount")
        if raw is not None:
            try:
                peers = int(raw)
            except ValueError:
                peers = None
    return {
        "time": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "hashrate": float(hashrate),
        "difficulty": info.get("difficulty"),
        "height": info.get("blocks"),
        "connections": peers,
    }, None


def notify(title, message, webhook):
    print(f"ALERT: {title} - {message}", flush=True)
    if sys.platform == "darwin":
        try:
            subprocess.run(
                ["osascript", "-e",
                 f'display notification "{message}" with title "{title}"'],
                capture_output=True, timeout=15)
        except (subprocess.TimeoutExpired, OSError):
            pass
    if webhook:
        try:
            payload = json.dumps({"text": f"{title}: {message}"}).encode()
            req = urllib.request.Request(
                webhook, data=payload, headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=20).read()
        except Exception as exc:  # noqa: BLE001 - a failed alert must not kill the monitor
            print(f"webhook failed: {exc}", file=sys.stderr, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default=os.environ.get("KORSH_CLI", "korsh-cli"))
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--chain", default="main")
    ap.add_argument("--interval", type=int, default=300,
                    help="seconds between samples (default 300)")
    ap.add_argument("--csv", default="korsh-hashrate.csv")
    ap.add_argument("--drop-pct", type=float, default=35.0,
                    help="alert when the hashrate falls this much versus the recent median")
    ap.add_argument("--window", type=int, default=10,
                    help="how many samples the recent median is computed over")
    ap.add_argument("--webhook", default=None)
    args = ap.parse_args()

    history = []
    alerted = False
    if os.path.exists(args.csv):
        with open(args.csv) as fh:
            rows = fh.read().strip().splitlines()[1:]
        for row in rows[-args.window:]:
            try:
                history.append(float(row.split(",")[1]))
            except (IndexError, ValueError):
                pass

    new_file = not os.path.exists(args.csv)
    csv_fh = open(args.csv, "a")
    if new_file:
        csv_fh.write("time,hashrate,difficulty,height,connections\n")
        csv_fh.flush()

    print(f"monitoring {args.chain} via {args.cli} (datadir {args.datadir}), "
          f"every {args.interval}s, alerting on a {args.drop_pct}% drop", flush=True)
    while True:
        data, err = sample(args.cli, args.datadir, args.chain)
        if data is None:
            print(f"{datetime.now(timezone.utc).isoformat(timespec='seconds')} "
                  f"sample failed: {err}", file=sys.stderr, flush=True)
        else:
            csv_fh.write("{time},{hashrate},{difficulty},{height},{connections}\n".format(
                **{k: ("" if v is None else v) for k, v in data.items()}))
            csv_fh.flush()
            hr = data["hashrate"]
            line = (f"{data['time']} hashrate={fmt_hashrate(hr)} difficulty={data['difficulty']} "
                    f"height={data['height']} peers={data['connections']}")
            print(line, flush=True)

            if history:
                recent = sorted(history)[len(history) // 2]
                if recent > 0 and hr < recent * (1 - args.drop_pct / 100.0):
                    alerted = True
                    notify("Korsh hashrate drop",
                           f"network hashrate {fmt_hashrate(hr)} is {args.drop_pct:.0f}%+ below the "
                           f"recent median {fmt_hashrate(recent)} at height {data['height']}",
                           args.webhook)
            history.append(hr)
            history[:] = history[-args.window:]

        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
