#!/usr/bin/env python3
"""Turn a synced node into ready-to-paste checkpoint entries for chainparams.cpp.

Checkpoints are the cheapest protection a young proof-of-work chain has against a
deep reorg: a node carrying them refuses any chain whose block at that height
differs. They are only useful once the chain has a stable history, so generate
them from a node you trust, at a spacing deep enough that the checkpoint is
irreversible in practice (a few thousand blocks is plenty for a 60 second chain).

    korsh-checkpoints.py --cli ./bin/korsh-cli --datadir ~/.korsh --every 5000
    korsh-checkpoints.py ... --from 100000 --out checkpoints.txt

Paste the printed lines into CMainParams::CMainParams() after the existing
`checkpointData = { ... }` opening brace, then rebuild and release.
"""
import argparse
import json
import subprocess
import sys


def cli(cli_path, datadir, chain, *args):
    cmd = [cli_path, f"-datadir={datadir}"]
    if chain:
        cmd.append(f"-chain={chain}")
    cmd.extend(args)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip() or out.stdout.strip())
    return out.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default="korsh-cli")
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--chain", default="main")
    ap.add_argument("--every", type=int, default=5000,
                    help="spacing between checkpoints in blocks (default 5000)")
    ap.add_argument("--from", dest="first", type=int, default=0,
                    help="first height to consider (default 0 = the genesis)")
    ap.add_argument("--last", type=int, default=None,
                    help="highest height to consider (default: leave this many "
                         "blocks of margin below the tip)")
    ap.add_argument("--margin", type=int, default=1000,
                    help="blocks to stay below the tip (default 1000)")
    ap.add_argument("--out", default=None, help="also write the entries to this file")
    args = ap.parse_args()

    info = json.loads(cli(args.cli, args.datadir, args.chain, "getblockchaininfo"))
    tip = int(info["blocks"])
    last = args.last if args.last is not None else max(0, tip - args.margin)
    print(f"# chain={args.chain} tip={tip} generating checkpoints up to {last} "
          f"every {args.every} blocks", file=sys.stderr)

    entries = []
    height = args.first
    while height <= last:
        blockhash = cli(args.cli, args.datadir, args.chain, "getblockhash", str(height))
        entries.append(f'                {{{height}, uint256S("0x{blockhash}")}},')
        print(f"# {height}: {blockhash}", file=sys.stderr)
        height += args.every

    if not entries:
        print("# no heights in range yet; nothing to checkpoint", file=sys.stderr)
        return 1

    text = "\n".join(entries)
    print(text)
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(text + "\n")
        print(f"# written to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
