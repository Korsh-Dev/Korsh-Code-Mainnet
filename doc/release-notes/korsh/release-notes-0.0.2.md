# Korsh Core v0.0.2 — KSH

**Network-identity release.** Korsh becomes fully Korsh-only: the P2P network
magic, the default ports and every user-visible string are now Korsh's own.
Nodes running v0.0.1 use the old Smartiecoin-era network identity, so they can
no longer see or connect to v0.0.2 nodes.

## Why update

| | v0.0.1 (old) | v0.0.2 (this release) |
|---|---|---|
| P2P magic (mainnet) | Smartiecoin legacy | `cb4b5348` ("KSH") |
| P2P port (mainnet) | 8383 | **9777** |
| RPC port (mainnet) | 8282 | **9776** |
| Signed-message magic | "DarkCoin Signed Message:\n" | **"Korsh Signed Message:\n"** |
| Bootstrap seed | 195.26.244.209:8383 | **195.26.244.209:9777** |
| Version string | v0.0.1 | **v0.0.2** |

## What did NOT change (on purpose)

- **The genesis block and the whole chain** — no reset, no reindex, no fork.
  Genesis is still `000017ec1f2f978429db70495ec7653aedca360e5f251bfe2a780ae5e731d7eb`,
  Block 0 is still `In honor of my uncle Satoshi Nakamoto`, and every block,
  balance and transaction mined so far is untouched.
- **Consensus and tokenomics** — YesPower 1.0 (N=256, r=8), 2 KSH block
  subsidy, 60 s blocks, 10,000,000 KSH cap, halving every 2,500,000 blocks,
  masternode collateral and payments, retarget every 20 blocks.
- **Addresses and wallets** — base58 prefixes are untouched and every platform
  keeps the exact Berkeley DB version v0.0.1 already used:
  macOS = BDB 4.8.30 (static), Linux = BDB 5.3 (system library, dynamic),
  Windows = BDB 6.2 (static). Existing `wallet.dat` files open with the new
  binaries — no conversion, no migration.
- **Mining** — the stratum endpoint (`:3333`) and the pool configuration are
  unchanged; pool miners do not need to do anything.

## Upgrading

**Node operators:** stop the node, replace the binaries, and update the ports
if they are pinned in `korsh.conf` (`port=9777`, `rpcport=9776`,
`externalip=<ip>:9777`). You can simply delete those three lines to use the
new defaults. Do **not** pass `-reindex` — the chain data is compatible as-is.

**Solo miners and tools:** the RPC endpoint is now `http://127.0.0.1:9776/` by
default. The bundled `korsh-miner` already defaults to it; if you run your own
tools, point them at the new port.

**Pool miners:** nothing changes — keep mining on the same stratum endpoint.

## Downloads

| Platform | File |
|---|---|
| Linux x86_64 | `korsh-0.0.2-linux-x86_64.tar.gz` (daemon, CLI, tools + Qt wallet) |
| macOS (Apple Silicon) | `korsh-0.0.2-macos-arm64.tar.gz` (daemon, CLI, tools + Qt wallet) |
| Windows x64 | `korsh-0.0.2-win64.zip` (daemon, CLI, tools + Qt wallet) |
| Checksums | `SHA256SUMS` |

## Verification (this release)

- Shipped binaries report `Korsh Core version v0.0.2` with Korsh-only credits.
- Binary string audit: zero "Smartiecoin" identifiers remain in the shipped
  binaries (headers, strings, ports and magic were scrubbed from the source).
- Mainnet genesis hash re-checked after the changes: unchanged
  (`000017ec…1d7eb`); no checkpoint, base58 prefix or spork was modified.
- Regtest smoke test on the built binaries before packaging; the Linux set is
  additionally smoke-tested on the Korsh seed node host.
