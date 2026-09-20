# Korsh Core [KSH]

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

The genesis block must be regenerated after the final consensus parameters are locked. The previous inherited genesis values are obsolete because Korsh now uses a 2 KSH initial subsidy, a 2,500,000-block halving interval, 70/30 reward distribution, and a new mainnet identity. The launch genesis will be mined with the current launch timestamp and recorded here together with its nonce, target, hash, and merkle root.

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

Korsh uses the **Yespower** proof-of-work algorithm, specifically optimized for fair CPU mining and resistant to centralized ASIC hardware.

You can solo mine directly with the node or point any Yespower-compatible CPU miner (such as `cpuminer-opt`) to your node's RPC or stratum pool.

To start built-in generation from the CLI:

```bash
./src/korsh-cli setgenerate true -1
```

---

## License

Korsh Core is released under the terms of the **MIT license**. See [COPYING](COPYING) for more information or visit [https://opensource.org/licenses/MIT](https://opensource.org/licenses/MIT).
