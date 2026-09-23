# Korsh explorer (lightweight fallback)

A small Flask + SQLite explorer written for this repo. It is **not** the explorer
running on the public VPS — that one is [eIquidus](https://github.com/team-exor/eiquidus),
which adds masternode pages, rich list, movement and network panels. This one is
kept as a dependency-free fallback: no MongoDB, no Node, one Python file.

It talks to the node through `korsh-cli` and never parses raw transactions:
everything comes from `getblock <hash> 2`, so Dash special transactions (ProTx,
CoinJoin) are decoded by the node itself.

## Run it

```sh
python3 -m venv .venv && .venv/bin/pip install flask

.venv/bin/python explorer.py \
  --cli /opt/korsh/bin/korsh-cli \
  --datadir /home/korsh/.korsh \
  --chain=main \
  --db /var/lib/korsh-explorer/explorer.sqlite \
  --host 127.0.0.1 --port 8090
```

`--chain` matters even for the client: each network has its own port and cookie
path, and without it the client reports "Could not locate RPC credentials".

Put nginx in front (`proxy_pass http://127.0.0.1:8090;`) with a Let's Encrypt
certificate, the same way the eIquidus host is served.

## What it indexes

An indexer thread follows the node's tip and stores blocks, transactions, the
per-address UTXO set (so address pages show balances) and a small address table.
The database lives in SQLite; delete the file to reindex from scratch — a chain
of a few tens of thousands of blocks rebuilds in minutes.
