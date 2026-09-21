# Korsh mainnet launch checklist

Operational steps to take v0.0.1 live. Everything in section 0 is already done and
verified; sections 1 to 4 are the launch itself; section 5 is what the **next**
release should carry.

## 0. Already done (verified before this document was written)

- Binaries for macOS (arm64), Windows (x86_64) and Linux (x86_64) built from the
  same commit, published on the v0.0.1 release together with `SHA256SUMS.txt`.
- Consensus is final for launch: YesPower 1.0 at **N=256, r=8** (256 KB working
  set), 60 s blocks, DGW retargeting every block with a 20-block window, 70/30
  miner/masternode split, 10,000,000 KSH cap, 2,500,000-block halving.
- Genesis mined for those parameters and verified (mainnet `000017ec1f2f978429db70495ec7653aedca360e5f251bfe2a780ae5e731d7eb`,
  regtest re-mined as well), no premine — mainnet sits at height 0.
- Sapling zkSNARK parameters embedded in the binaries on all three platforms, so
  no manual `params/` handling is needed anywhere.
- GUI only offers what the network can run (no Governance / InstantSend /
  ChainLocks / Evo controls while quorums and budget payments are disabled).
- Smoke set on the final build: `wallet_basic`, `mining_basic`, `rpc_help`,
  `feature_filelock`, `feature_governance` pass; the two LLMQ/ChainLocks tests
  fail by design (documented in the release notes).

## 1. Start the first node(s)

```sh
mkdir -p ~/.korsh
./bin/korshd -daemon
./bin/korsh-cli getblockchaininfo      # chain: main, blocks: 0
```

Mainnet ports: **P2P 8383**, **RPC 8282**.

**A public seed node is already running** at `195.26.244.209:8383` (same VPS as
the explorer), and the binaries from this release carry it as a fixed seed, so a
fresh install connects on its own about a minute after starting. The machine runs
`korshd` under systemd as the `korsh` user with the datadir in `/home/korsh/.korsh`;
its RPC stays on localhost.

On a minimal Debian/Ubuntu install the Linux tarball also needs its runtime
libraries:

```sh
apt-get install -y libdb5.3t64 libdb5.3++t64 libminiupnpc17 libnatpmp1 libevent-2.1-7t64 libevent-pthreads-2.1-7t64
```

## 2. Mine the first blocks

The daemon can mine with its own CPU miner:

```sh
./bin/korsh-cli createwallet launch
ADDR=$(./bin/korsh-cli getnewaddress)
./bin/korsh-cli generatetoaddress 10 "$ADDR"
```

Any external miner must be built for **YesPower 1.0, N=256, r=8** — miners built
for the previous r=32 parameters produce invalid blocks and will be rejected.

## 3. Let other nodes find the network

The binaries ship with the first seed node hardcoded (`vFixedSeeds` in
`src/chainparams.cpp`, BIP155 form of `195.26.244.209:8383`), so a new install
needs no flags: roughly a minute after start the node loads the fixed seed and
connects. There is no DNS seed yet, so if that node is retired the list has to be
updated in a release; adding a DNS seed (or more fixed seeds) is a one-line change
plus a rebuild.

## 3b. The block explorer

<https://explorer.195-26-244-209.sslip.io/> — eIquidus (Node + MongoDB) reading
the same node's RPC. It has block/transaction/address pages plus **masternodes**,
rich list, movement, network panels and public JSON APIs. Temporary hostname
(`sslip.io` encodes the IP) until a domain is bought.

Operational notes for whoever maintains it:

- Code: `/opt/korsh-explorer-eiquidus` (clone of `team-exor/eiquidus`), service
  `korsh-explorer-eiquidus.service`, database `explorerdb` in MongoDB.
- Sync: `/etc/cron.d/korsh-explorer` runs `scripts/sync.js index` every minute and
  `peers`/`masternodes` every five minutes.
- The masternode table fills from `/ext/getmasternodelist`, which the sync
  populates from the node's masternode RPC.
- Theme/coin name/logo live in `settings.json` (`theme` accepts any Bootswatch
  theme, e.g. `slate`, `cyborg`, `darkly`).
- The older lightweight explorer written for this repo is still in
  `contrib/explorer/` as a dependency-free fallback (Flask + SQLite); it is not
  running.

Two install traps worth knowing before touching this stack a second time:

- **eIquidus needs Node >= 20.19** (`scripts/prestart.js` hard-fails on 18, which
  is what Ubuntu 24.04 ships). Install NodeSource's 20.x first, or `npm start`
  (which runs `bin/cluster`, not `app.js`) dies instantly and the unit just says
  "Deactivated successfully".
- **The Mongo user must live in the database named in `settings.json`**
  (`explorerdb`), not in `admin`. The driver resolves `authSource` from the URI's
  database, so a user created in `admin` authenticates fine with `mongosh` and
  still gets `AuthenticationFailed (code 18)` from the app.

## 4. Protect the young chain

- Difficulty already reacts every block (DGW), so a hashrate spike is absorbed
  within the 20-block window.
- Watch the hashrate with `contrib/korsh-hashrate-monitor.py` and alert on drops.
- Do **not** list KSH on an exchange until the honest hashrate is large enough
  that renting a majority costs more than the attack is worth. A 51% reorg on an
  unlisted chain has nothing to steal; on a listed one it does.
- After a few thousand blocks, freeze checkpoints with
  `contrib/korsh-checkpoints.py` and ship them (section 5).

## 5. Next release should carry

- **Checkpoints**: paste the output of `contrib/korsh-checkpoints.py` into
  `checkpointData` in `src/chainparams.cpp` (mainnet) and rebuild. This is the
  cheapest protection against deep reorgs and needs no quorums.
- **Fixed seeds or a DNS seed**: fill `vFixedSeeds` / `vSeeds` in
  `src/chainparams.cpp` with the chosen seed nodes so new installs find the
  network without `-addnode`.
- Optional: sign and notarise the macOS binaries with a **Developer ID
  Application** certificate (the Apple Distribution certificate in the account
  today cannot notarise direct downloads).
- Optional: a Windows installer (NSIS) to replace the plain zip.
