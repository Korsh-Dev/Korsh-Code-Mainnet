# Korsh Core [KSH]

<p align="center">
  <img src="doc/korsh-logo.png" alt="Korsh (KSH)" width="150">
</p>

[![Release](https://img.shields.io/badge/release-v0.0.1-blue.svg)](https://github.com/ELPilotPR/Korsh-Core/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Korsh Core** is the reference implementation of Korsh (KSH), a decentralized, peer-to-peer cryptocurrency focused on security, fast settlement, ASIC-resistant CPU mining, and community governance.

Korsh is a fork of Smartiecoin Core; Smartiecoin attribution is retained in source credits and licenses.

---

## Network Specifications

| Parameter | Specification |
| :--- | :--- |
| **Coin Name** | Korsh |
| **Ticker** | **KSH** |
| **Consensus Model** | **Proof-of-Work (YesPower CPU) with deterministic masternodes; quorums and Dash Platform disabled** |
| **Block Time** | 60 seconds (1 minute) |
| **Initial Block Subsidy** | **2 KSH** (70% miner / 30% masternodes) |
| **Halving Interval** | 2,500,000 blocks |
| **Maximum Supply Cap** | 10,000,000 KSH |
| **Estimated Emission Schedule** | 5,000,000 KSH in era 1; 7,500,000 KSH in era 2; 8,750,000 KSH in era 3; asymptotically approaches 10,000,000 KSH |
| **Regular Masternode Collateral** | **1,500 KSH** |
| **Evo Masternode Collateral** | **7,500 KSH**, disabled initially and reserved for future activation by block height |
| **Masternode Payments** | 30% of the block subsidy; regular masternodes remain enabled |
| **Difficulty Retarget** | Targeted every 20 blocks using a 20-minute timespan |
| **Optional Services** | Quorums, InstantSend, ChainLocks, Dash Platform, governance, superblocks and Evo masternodes disabled initially; retained for future height-based activation |
| **Default P2P Port** | `8383` |
| **RPC Default Port** | `8382` |

---

## Genesis Block Details

The Korsh mainnet genesis block was mined with the final launch parameters:

```text
Timestamp Phrase: "In honor of my uncle Satoshi Nakamoto"
Unix Timestamp (nTime): 1789885960 (September 20, 2026)
Nonce (nNonce): 26429
Difficulty (nBits): 0x1e3fffff
Genesis Reward: 2 KSH

Genesis Hash:
000017ec1f2f978429db70495ec7653aedca360e5f251bfe2a780ae5e731d7eb

Merkle Root:
1dd60dd52dda0fd4b6912baca209aa327ae8b4825a23415b0a4356585897d073
```

The node verifies this genesis at startup and reports it as mainnet block 0.

---

## Building from Source

### 1. Prerequisites (Ubuntu / Debian / WSL2)

Install the required build tools and libraries:

```bash
sudo apt update
sudo apt install -y build-essential libtool autotools-dev automake pkg-config \
    bsdmainutils python3 libssl-dev libevent-dev libboost-all-dev \
    libdb5.3++-dev libdb5.3-dev libsqlite3-dev libzmq3-dev \
    libgmp-dev libsodium-dev cargo rustc dos2unix
```

### 2. Clone the Repository

```bash
git clone https://github.com/ELPilotPR/Korsh-Core.git
cd Korsh-Core
```

### 3. Configure and Compile

```bash
# Generate configuration scripts
./autogen.sh

# Configure build (headless daemon without GUI/tests for maximum speed)
./configure --without-gui --disable-tests --disable-bench --with-incompatible-bdb --disable-man

# Build binaries using all CPU cores
make -j$(nproc)
```

The compiled binaries will be located in `src/`:
* `korshd` / `korshd` — Headless full node daemon
* `korsh-cli` / `korsh-cli` — RPC command-line tool
* `korsh-tx` / `korsh-tx` — Transaction creation tool

---

## Running a Korsh Node

### Basic Configuration (`korsh.conf`)

Create your data directory configuration file at `~/.korsh/korsh.conf`:

```ini
# Network settings
server=1
daemon=1
listen=1
maxconnections=64

# RPC settings
rpcuser=your_rpc_username
rpcpassword=your_secure_password
rpcport=8382
rpcallowip=127.0.0.1
```

### Start the Daemon

```bash
./src/korshd -daemon
```

### Query Node Information

```bash
./src/korsh-cli getblockchaininfo
./src/korsh-cli getnetworkinfo
```

---

## Mining Korsh (Yespower)

Korsh uses the **Yespower** proof-of-work algorithm (YesPower 1.0) tuned so that
CPU mining is practical on ordinary hardware — old desktops, laptops and phones —
while GPUs and ASICs still gain nothing, because the working set does not fit in
their per-thread memory.

### Cache footprint

The memory a single hash needs is `128 * N * r` bytes plus roughly 100 KB of
S-boxes. Korsh runs **N = 256, r = 8**, the smallest configuration Yespower
accepts, which is what keeps it inside the cache of weak hardware:

| Parameter | Region per hash | Plus S-boxes |
|---|---|---|
| `N = 256, r = 32` (previous parameters) | 1 MB | ~100 KB |
| **`N = 256, r = 8` (current)** | **256 KB** | **~100 KB** |

This matters more than it looks: a 1 MB region does not fit in the 512 KB of L2
per core of, for example, an AMD EPYC 9554, so every hash ran from L3 there
(measured at ~2.05 kH/s per thread, scaling linearly with thread count). At
256 KB the region is served from L2 instead, which is what makes the chain
reachable for older CPUs and mobile devices.

Anyone mining Korsh must use a miner built for **YesPower 1.0, N=256, r=8 with
no personalisation**. A miner compiled for the previous r=32 parameters produces
blocks that the network rejects.

### Solo mining

The node can mine on its own:

```bash
./src/korsh-cli createwallet mining
ADDR=$(./src/korsh-cli getnewaddress)
./src/korsh-cli generatetoaddress 1 "$ADDR"
```

Any external miner must talk to the node's RPC and implement the parameters
above.

---

## License

Korsh Core is released under the terms of the **MIT license**. See [COPYING](COPYING) for more information or visit [https://opensource.org/licenses/MIT](https://opensource.org/licenses/MIT).
