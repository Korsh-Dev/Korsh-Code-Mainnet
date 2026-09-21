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

Mainnet ports: **P2P 8383**, **RPC 8282**. Open 8383/tcp on the seed machine(s)
so others can reach them.

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

There are **no DNS seeds and no fixed seeds** in the binaries yet, so peers have
to be introduced by hand until the next release:

- Tell users to start their node with `-addnode=<seed-ip>:8383`.
- Publish the seed list wherever the launch is announced, and keep at least two
  always-on nodes.

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
