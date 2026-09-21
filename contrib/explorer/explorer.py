#!/usr/bin/env python3
"""A small Korsh block explorer.

Written for a Dash-derived chain, which is why it never parses raw transactions
itself: everything comes from the node with `getblock <hash> 2`, so special
transactions (ProTx, CoinJoin, asset locks) are handled by the node's own
decoder instead of a Bitcoin-era parser that would choke on them.

Runs two things in one process:

  * a Flask app (blocks, transactions, addresses, search, JSON stats)
  * an indexer thread that follows the node's tip and keeps a SQLite database
    with the blocks, transactions and the per-address UTXO set that the address
    pages need

The node is queried through `korsh-cli`, so the RPC credentials never leave the
node's own config file and this app needs no RPC library.

Usage:
    explorer.py --cli /opt/korsh/bin/korsh-cli --datadir /home/korsh/.korsh \
                --db /var/lib/korsh-explorer/explorer.sqlite --port 8090
"""
import argparse
import json
import os
import sqlite3
import subprocess
import threading
import time
from datetime import datetime, timezone

from flask import Flask, abort, jsonify, redirect, render_template, request, url_for

app = Flask(__name__)
STATE = {"cli": None, "datadir": None, "chain": "main", "db": None, "started": None}
DB_LOCK = threading.Lock()


# --------------------------------------------------------------------------- #
# node access
# --------------------------------------------------------------------------- #
def cli(*args, timeout=60):
    # The chain flag matters even for the client: every network has its own port
    # and cookie path, so without it the client looks for the mainnet cookie and
    # reports "Could not locate RPC credentials".
    cmd = [STATE["cli"], f"-datadir={STATE['datadir']}", f"-chain={STATE['chain']}"] + [str(a) for a in args]
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip() or out.stdout.strip())
    text = out.stdout.strip()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def db():
    conn = sqlite3.connect(STATE["db"], timeout=30)
    conn.row_factory = sqlite3.Row
    return conn


SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS blocks (
    height INTEGER PRIMARY KEY, hash TEXT UNIQUE, prev TEXT, time INTEGER,
    size INTEGER, tx_count INTEGER, difficulty REAL, chainwork TEXT
);
CREATE TABLE IF NOT EXISTS txs (
    txid TEXT PRIMARY KEY, height INTEGER, position INTEGER, time INTEGER,
    total_out INTEGER, fee INTEGER
);
CREATE TABLE IF NOT EXISTS outputs (
    txid TEXT, vout INTEGER, address TEXT, value INTEGER,
    spent_by TEXT, spent_vin INTEGER, height INTEGER,
    PRIMARY KEY (txid, vout)
);
CREATE TABLE IF NOT EXISTS addresses (
    address TEXT PRIMARY KEY, balance INTEGER DEFAULT 0,
    received INTEGER DEFAULT 0, tx_count INTEGER DEFAULT 0, first_height INTEGER
);
CREATE INDEX IF NOT EXISTS idx_outputs_address ON outputs(address);
CREATE INDEX IF NOT EXISTS idx_txs_height ON txs(height DESC);
CREATE INDEX IF NOT EXISTS idx_txs_time ON txs(time DESC);
"""


def init_db():
    with DB_LOCK, db() as conn:
        conn.executescript(SCHEMA)


def meta_get(key, default=None):
    with DB_LOCK, db() as conn:
        row = conn.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return row["value"] if row else default


def meta_set(key, value):
    with DB_LOCK, db() as conn:
        conn.execute("INSERT INTO meta(key,value) VALUES(?,?) "
                     "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, str(value)))


def addresses_of(script_pubkey):
    """Addresses a decoded scriptPubKey pays to, if any."""
    if script_pubkey.get("type") == "nulldata":
        return []
    addresses = script_pubkey.get("addresses") or []
    if not addresses and script_pubkey.get("address"):
        addresses = [script_pubkey["address"]]
    return [a for a in addresses if a]


def index_block(block):
    """Insert one decoded block (getblock <hash> 2 output) plus its outputs."""
    height = block["height"]
    fee_total = 0
    with DB_LOCK, db() as conn:
        conn.execute("INSERT OR REPLACE INTO blocks"
                     "(height,hash,prev,time,size,tx_count,difficulty,chainwork) "
                     "VALUES(?,?,?,?,?,?,?,?)",
                     (height, block["hash"], block.get("previousblockhash"), block["time"],
                      block.get("size", 0), len(block["tx"]), block.get("difficulty"),
                      block.get("chainwork")))
        for position, tx in enumerate(block["tx"]):
            txid = tx["txid"]
            # resolve the inputs: mark the previous outputs as spent
            in_value = 0
            for vin in tx.get("vin", []):
                if "txid" in vin and "vout" in vin:
                    row = conn.execute("SELECT value,address FROM outputs WHERE txid=? AND vout=?",
                                       (vin["txid"], vin["vout"])).fetchone()
                    if row:
                        in_value += row["value"]
                        conn.execute("UPDATE outputs SET spent_by=?, spent_vin=? "
                                     "WHERE txid=? AND vout=?",
                                     (txid, vin.get("n"), vin["txid"], vin["vout"]))
                        if row["address"]:
                            conn.execute("UPDATE addresses SET balance=balance-? WHERE address=?",
                                         (row["value"], row["address"]))
            out_value = 0
            for vout in tx.get("vout", []):
                value = int(round(vout["value"] * 1e8))
                out_value += value
                for address in addresses_of(vout.get("scriptPubKey", {})):
                    conn.execute(
                        "INSERT OR IGNORE INTO addresses(address,balance,received,tx_count,first_height) "
                        "VALUES(?,0,0,0,?)", (address, height))
                    conn.execute(
                        "UPDATE addresses SET balance=balance+?, received=received+?, "
                        "tx_count=tx_count+1 WHERE address=?", (value, value, address))
                conn.execute("INSERT OR REPLACE INTO outputs"
                             "(txid,vout,address,value,spent_by,spent_vin,height) VALUES(?,?,?,?,NULL,NULL,?)",
                             (txid, vout["n"], (addresses_of(vout.get("scriptPubKey", {})) or [None])[0],
                              value, height))
            fee = None
            if position > 0 and in_value:
                fee = in_value - out_value
                fee_total += fee
            conn.execute("INSERT OR REPLACE INTO txs(txid,height,position,time,total_out,fee) "
                         "VALUES(?,?,?,?,?,?)",
                         (txid, height, position, block["time"], out_value, fee))
    return fee_total


def indexer():
    """Follow the node's tip and index new blocks until it catches up."""
    while True:
        try:
            tip = int(cli("getblockcount"))
            have = int(meta_get("height", -1))
            if have < tip:
                for height in range(have + 1, tip + 1):
                    blockhash = cli("getblockhash", height)
                    block = cli("getblock", blockhash, 2, timeout=120)
                    index_block(block)
                    meta_set("height", height)
                continue  # immediately look for more
        except Exception as exc:  # noqa: BLE001 - the indexer must survive node restarts
            app.logger.warning("indexer: %s", exc)
        time.sleep(5)


# --------------------------------------------------------------------------- #
# views
# --------------------------------------------------------------------------- #
@app.template_filter("ksh")
def ksh_filter(satoshis):
    try:
        return f"{int(satoshis) / 1e8:,.8f}".rstrip("0").rstrip(".") or "0"
    except (TypeError, ValueError):
        return "0"


@app.template_filter("ago")
def ago_filter(timestamp):
    try:
        delta = int(time.time()) - int(timestamp)
    except (TypeError, ValueError):
        return ""
    for unit, size in (("d", 86400), ("h", 3600), ("m", 60), ("s", 1)):
        if delta >= size:
            return f"{delta // size}{unit} ago"
    return "just now"


def network_stats():
    info = cli("getblockchaininfo")
    mining = cli("getmininginfo")
    try:
        peers = int(cli("getconnectioncount"))
    except Exception:  # noqa: BLE001
        peers = None
    supply = None
    if info.get("blocks", 0) > 0:
        try:
            supply = cli("gettxoutsetinfo")["total_amount"]
        except Exception:  # noqa: BLE001
            supply = None
    return {
        "height": info.get("blocks"),
        "difficulty": info.get("difficulty"),
        "hashrate": mining.get("networkhashps"),
        "peers": peers,
        "supply": supply,
        "bestblockhash": info.get("bestblockhash"),
        "chain": info.get("chain"),
    }


@app.route("/")
def home():
    try:
        stats = network_stats()
    except Exception as exc:  # noqa: BLE001
        return render_template("down.html", error=str(exc)), 503
    with DB_LOCK, db() as conn:
        blocks = conn.execute("SELECT * FROM blocks ORDER BY height DESC LIMIT 15").fetchall()
        txs = conn.execute("SELECT * FROM txs ORDER BY height DESC, position DESC LIMIT 12").fetchall()
        indexed = conn.execute("SELECT COUNT(*) c FROM addresses").fetchone()["c"]
    return render_template("index.html", stats=stats, blocks=blocks, txs=txs, indexed=indexed)


@app.route("/block/<ident>")
def block_view(ident):
    try:
        if ident.isdigit():
            blockhash = cli("getblockhash", int(ident))
        else:
            blockhash = ident
        block = cli("getblock", blockhash, 2, timeout=120)
    except Exception as exc:  # noqa: BLE001
        abort(404, description=str(exc))
    return render_template("block.html", block=block)


@app.route("/tx/<txid>")
def tx_view(txid):
    try:
        tx = cli("getrawtransaction", txid, 1)
    except Exception as exc:  # noqa: BLE001
        abort(404, description=str(exc))
    with DB_LOCK, db() as conn:
        row = conn.execute("SELECT * FROM txs WHERE txid=?", (txid,)).fetchone()
    return render_template("tx.html", tx=tx, row=row)


@app.route("/address/<address>")
def address_view(address):
    with DB_LOCK, db() as conn:
        info = conn.execute("SELECT * FROM addresses WHERE address=?", (address,)).fetchone()
        outputs = conn.execute("SELECT * FROM outputs WHERE address=? ORDER BY height DESC LIMIT 50",
                               (address,)).fetchall()
    if info is None and not outputs:
        abort(404, description="Address not seen in any indexed block yet")
    return render_template("address.html", address=address, info=info, outputs=outputs)


@app.route("/search")
def search():
    query = (request.args.get("q") or "").strip()
    if not query:
        return redirect(url_for("home"))
    if query.isdigit():
        return redirect(url_for("block_view", ident=query))
    if query.startswith("0x"):
        query = query[2:]
    if len(query) == 64:
        with DB_LOCK, db() as conn:
            if conn.execute("SELECT 1 FROM blocks WHERE hash=?", (query,)).fetchone():
                return redirect(url_for("block_view", ident=query))
            if conn.execute("SELECT 1 FROM txs WHERE txid=?", (query,)).fetchone():
                return redirect(url_for("tx_view", txid=query))
        return redirect(url_for("tx_view", txid=query))
    return redirect(url_for("address_view", address=query))


@app.route("/api/stats")
def api_stats():
    try:
        return jsonify(network_stats())
    except Exception as exc:  # noqa: BLE001
        return jsonify({"error": str(exc)}), 503


def main():
    ap = argparse.ArgumentParser(description="Korsh block explorer")
    ap.add_argument("--cli", default="/opt/korsh/bin/korsh-cli")
    ap.add_argument("--datadir", default="/home/korsh/.korsh")
    ap.add_argument("--chain", default="main")
    ap.add_argument("--db", default="/var/lib/korsh-explorer/explorer.sqlite")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8090)
    args = ap.parse_args()

    STATE.update(cli=args.cli, datadir=args.datadir, chain=args.chain, db=args.db,
                 started=datetime.now(timezone.utc).isoformat(timespec="seconds"))
    os.makedirs(os.path.dirname(args.db), exist_ok=True)
    init_db()
    threading.Thread(target=indexer, daemon=True).start()
    app.run(host=args.host, port=args.port)


if __name__ == "__main__":
    main()
