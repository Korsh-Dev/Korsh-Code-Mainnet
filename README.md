# Korsh Core [KSH]

[![Release](https://img.shields.io/badge/release-v0.0.1-blue.svg)](https://github.com/MrGasparin/Korsh-Mainnet/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Korsh Core** is the reference implementation of Korsh (KSH), a decentralized, peer-to-peer cryptocurrency focused on security, fast settlement, ASIC-resistant CPU mining, and community governance.

---

## Network Specifications

| Parameter | Specification |
| :--- | :--- |
| **Coin Name** | Korsh |
| **Ticker** | **KSH** |
| **PoW Algorithm** | **Yespower** (CPU-friendly, ASIC/GPU resistant) |
| **Block Time** | 60 seconds (1 minute) |
| **Initial Block Subsidy** | 50 KSH |
| **Halving Interval** | 1,000,000 blocks |
| **Maximum Supply Cap** | 10,000,000 KSH |
| **Default P2P Port** | `8383` |
| **Default Platform P2P Port** | `29256` |
| **RPC Default Port** | `8382` |

---

## Genesis Block Details

The Korsh Mainnet genesis block was established with the following cryptographic parameters:

```
Timestamp Phrase: "In honor of my uncle Satoshi Nakamoto"
Unix Timestamp (nTime): 1789866060
Nonce (nNonce): 402962
Difficulty (nBits): 0x1e3fffff
Genesis Reward: 10 KSH

Genesis Hash:
0x0000216e9ac922735ea501c032ad1d8e8bc1a3f84c1f798790c1d38233e07010

Merkle Root:
0x7063d1c6460801869eeef0b328c04ad5012a018f1268c0bec25dde15e7046bcd
```

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
git clone https://github.com/MrGasparin/Korsh-Mainnet.git
cd Korsh-Mainnet
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
* `korshd` / `smartiecoind` — Headless full node daemon
* `korsh-cli` / `smartiecoin-cli` — RPC command-line tool
* `korsh-tx` / `smartiecoin-tx` — Transaction creation tool

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
