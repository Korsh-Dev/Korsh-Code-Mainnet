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
  regtest re-mined as well), with no premine.
- Sapling zkSNARK parameters ship with every release package. The macOS app
  stores them under `Korsh-Qt.app/Contents/Resources/params`; its CLI/daemon
  archive also has a top-level `params/` directory. Other platform builds can
  extract their embedded copies on first start.
- GUI only offers what the network can run (no Governance / InstantSend /
  ChainLocks / Evo controls while quorums and budget payments are disabled).
- Smoke set on the final build: `wallet_basic`, `mining_basic`, `rpc_help`,
  `feature_filelock`, `feature_governance` pass; the two LLMQ/ChainLocks tests
  fail by design (documented in the release notes).

## 1. Start the first node(s)

```sh
mkdir -p ~/.korshcore
./bin/korshd -daemon
./bin/korsh-cli getblockchaininfo      # chain: main, blocks: 0
```

Mainnet ports: **P2P 9777**, **RPC 9776**, **Tor Onion 9775**.

Mainnet chain parameters contain fixed peer addresses `195.26.244.209:9777` and
`185.251.19.161:9777`, plus the DNS seed hostname `seed.korsh.org`. If peer
discovery is unavailable, start with `-addnode=<reachable-peer>:9777`. Keep RPC
bound to localhost unless remote access is explicitly secured.

The v0.0.6 Linux archive includes a `README.txt` with the Ubuntu 22.04 runtime
package list and glibc baseline. Other distributions need equivalent shared
libraries; use `ldd` on the packaged executables to check the local runtime.

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

The binaries ship with two fixed peers and the DNS seed `seed.korsh.org`
(`vFixedSeeds` and `vSeeds` in `src/chainparams.cpp`). If those endpoints change,
update the chain parameters and release a new build; `-addnode` remains available
for operators who need to supply a reachable peer manually.

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

All visible branding lives in `settings.json` (it accepts `//` comments, so a
plain `json.load` fails — strip them first): `shared_pages.logo` is the header
panel image, `shared_pages.page_header.page_title_image.image_path` is the small
image that rotates next to each page title, `shared_pages.page_footer.powered_by_text`
is the footer's "powered by" HTML (empty string removes it) and
`shared_pages.page_footer.social_links` is the footer's icon row (empty array
removes it). Restart the unit after editing.

## 3c. The website

<https://korsh.195-26-244-209.sslip.io/> — the Next.js 16 landing page (React 19,
three.js), served by nginx from a systemd unit.

- Code: `/opt/korsh-web` (source only; `node_modules` and `.next` are not shipped —
  the macOS build in the source zip is unusable on Linux). User `korshweb`, unit
  `korsh-web.service`, listening on `127.0.0.1:3000`.
- To update: replace the source files, then
  `su korshweb -s /bin/bash -c "cd /opt/korsh-web && npm ci && npm run build"` and
  `systemctl restart korsh-web`. The build is native to the server, so an update
  never needs a rebuild on a dev machine.
- nginx here is **1.24**, which does not accept the newer `http2 on;` directive —
  use `listen 443 ssl http2;`. The rejected config still passes `nginx -t` on the
  *old* file and only shows up at reload time, so always check `nginx -T` output
  (the effective config) after editing a vhost.

Users who want more than the wallet's single-threaded `generatetoaddress` can use the
CPU miner in `contrib/korsh-miner/` (shipped as `korsh-miner-0.0.1-win64.zip` in the
release): `korsh-miner <address> --threads N`. It is the korshcore fork's miner with
`KYP_R` set to 8 and a `_WIN32` port (affinity via `SetThreadAffinityMask`, `WSAPoll`,
`Sleep`-based sleeps). Its `--selftest` must print `identical to the reference` —
anything else means it is hashing the wrong consensus. It refuses templates with
masternode or superblock payouts, so it stops once a masternode registers; after that
use the wallet's `generatetoaddress`.

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
