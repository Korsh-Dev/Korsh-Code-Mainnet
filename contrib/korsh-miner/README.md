# korsh-miner

Solo and pool CPU miners for Korsh (Yespower 1.0, N=256, r=8). All of them hash with the node's own
`yespower_hash()` and only build and submit ordinary blocks or shares, so they are fully compatible with the
official consensus rules.

| Tool | What it is |
| :--- | :--- |
| `korsh-miner` (C, recommended) | Native miner. Solo mode (talks to your node over RPC) **and Stratum client** for pools. No Python needed, threads pinned to cores. Uses a two-way interleaved Yespower kernel: about 60 % faster than the Python miner in testing (83 kH/s vs 51.6 kH/s). |
| `korsh_miner.py` + `libyp.so` | Python solo miner with a native nonce scanner; easier to read and modify. |
| `korsh_stratum_pool.py` | **Reference Stratum pool** for testing and for pool developers. Not a production pool. |

## Build

```bash
contrib/korsh-miner/build.sh
```

Needs `gcc`, `libcurl`, `jansson` and OpenSSL development packages (see `contrib/install-deps-*.sh`).
It builds `korsh-miner` and `libyp.so`.

## Solo mining

```bash
bin/korsh-cli getnewaddress
contrib/korsh-miner/korsh-miner <address> --threads 8
```

Options: `--threads N` (default: CPU count minus 2), `--conf FILE`, `--rpc URL`, `--no-pin`, `--bench [seconds]`,
`--selftest`, `--ways 1|2` (kernel; default: two-way when it is proven identical to the reference).
RPC credentials come from `korsh.conf`.

## Pool mining (Stratum v1)

```bash
contrib/korsh-miner/korsh-miner --stratum stratum+tcp://pool.example.org:3333 \
    --user <your address>.rig1 --pass x --threads 8
```

No local node or `korsh.conf` is needed in this mode. The miner reconnects automatically, honours
`mining.set_difficulty`, `mining.set_extranonce` and `client.reconnect`, and reports accepted/rejected shares.

### Protocol conventions (a pool must match these)

| Item | Convention |
| :--- | :--- |
| Difficulty 1 | `0x0000ffff00..00` (the scrypt/Yespower convention). Use `--diff1 bitcoin` for `0x00000000ffff00..00`. Share target = diff1 / difficulty. |
| `mining.notify` | `[job_id, prevhash, coinb1, coinb2, merkle_branch, version, nbits, ntime, clean]` |
| `prevhash` | The previous block hash with every 4-byte word byte-swapped (standard Stratum v1). |
| `coinbase` | `coinb1 + extranonce1 + extranonce2 + coinb2`; its double-SHA256 is folded with `merkle_branch` to get the root. |
| `version`, `nbits`, `ntime` | Hex of the numeric header field (`%08x`). |
| `mining.submit` | `[user, job_id, extranonce2, ntime, nonce]`, `ntime` and `nonce` as `%08x`. |

Whether a share is also a block is decided by the node (`submitblock`) on the pool side; the miner never
needs to know.

### Try it locally with the reference pool

```bash
# terminal 1: needs a running node (with at least one peer, see below)
contrib/korsh-miner/korsh_stratum_pool.py <pool address> --listen 127.0.0.1:3333 --ease 16
# terminal 2
contrib/korsh-miner/korsh-miner --stratum stratum+tcp://127.0.0.1:3333 --user <address>.test
```

`--ease N` makes shares N times easier than a block so that shares show up often; `--ease 1` makes every
share a block. The reference pool validates shares with the node's `yespower_hash()` and submits blocks with
`submitblock`. It has no accounting, payouts, authentication or DoS protection.

Tested end to end: shares accepted, blocks accepted by the node, and a block containing a real mempool
transaction (which exercises the merkle branch) confirmed on chain.

## Performance and tuning

Measured with `korsh-miner --bench` on an AMD EPYC 9554 VPS (30 vCPUs), with the consensus parameters
(Yespower 1.0, N=256, r=8, about 256 KB of memory per hash). Every variant printed the same self-test digest, i.e. the
same hashes as the consensus code.

### Two-way interleaved kernel (default): about +35 %

One Yespower hash is a long chain of dependent steps: each pwxform round needs the previous round's result before it
can index the S-boxes, so a core mostly waits (about 1.75 instructions per cycle on a CPU that can issue several
times more). `korsh-yp2.c` computes **two independent hashes at once** and weaves their instructions together, so the
core has twice as many independent chains to overlap.

| Kernel | 1 thread | 8 threads | 30 threads |
| :--- | ---: | ---: | ---: |
| Reference, one hash at a time (`--ways 1`) | 2.05 kH/s | 16.4 kH/s | 61.6 kH/s |
| Two-way interleaved (`--ways 2`, default) | 2.79 kH/s | 22.3 kH/s | 83.0 kH/s |

How it stays correct: the kernel does not replace consensus code. It includes the node's own `yespower-opt.c`
(unmodified) and each of the two instances executes exactly the reference's operations in the reference's order.
Every start-up the miner compares the two-way kernel with the reference on random headers and only uses it when the
results are identical (otherwise it silently falls back to one hash at a time). `--selftest` runs a bigger check, and
the integration test confirms that blocks mined with it are accepted by the node. If the node's Yespower parameters
ever change, the check fails and the miner keeps working with the reference kernel until `korsh-yp2.c` is updated.

### Other build variants (all within measurement noise)

| Build variant (reference kernel, 28 threads) | Hashrate |
| :--- | ---: |
| SSE2 only (`-march=x86-64`) | 57.6 kH/s |
| AVX (`-mavx`) | 57.2 kH/s |
| AVX2 (`-mavx2`) | 57.2 kH/s |
| `-march=native` (default) | 57.3 kH/s |
| `-march=znver4 -mtune=znver4` | 57.6 kH/s |
| `-march=native` + 2 MB huge pages | 57.8 kH/s |

What this shows:

- **Use every core.** Throughput scales linearly with threads (no memory contention between threads).
- **The instruction set level does not matter.** The Yespower kernel is written with 128-bit vectors, so building with
  AVX2 or CPU-specific flags gives no measurable speed-up. The default `-march=native` is fine; for a binary that must
  run on other machines use `KORSH_CFLAGS="-O3 -march=x86-64-v2"`.
- **Huge pages are optional.** The upstream allocator only requests huge pages for regions of 12 MB or more, so a
  stock build never uses them for Korsh's 256 KB regions. `KORSH_HUGEPAGES=1 ./build.sh` builds a miner that does, and
  `setup-hugepages.sh` reserves the pages. The gain here is about +0.4 %; it can be larger on CPUs with small TLBs.
  Reserved pages are removed from the memory available to everything else.

Measure your own machine (stop other miners first):

```bash
contrib/korsh-miner/korsh-miner --bench 10 --threads 8 [--ways 1|2]   # hashrate, kernel, huge page use, digest
contrib/korsh-miner/bench.sh                                          # compares build variants and both kernels
contrib/korsh-miner/korsh-miner --selftest                            # digest + two-way vs reference check
```

## Node requirements

- `getblocktemplate` refuses to answer while the node is in initial block download or has **no peers**.
  On a brand-new chain with no other Korsh nodes, run a **local second node** connected to the first
  (see `contrib/systemd/korshd-peer.service` and [`docs/SYSTEMD.md`](../../docs/SYSTEMD.md)) and set `maxtipage=999999999`
  on the first node until blocks are produced.
- The template must not require masternode or superblock payments (disabled on Korsh).
- Templates without segwit expose transactions as `hash` (not `txid`); the miners and the reference pool handle both.

## Why not `cpuminer-opt`?

Its Yespower implementation produced hashes the node rejected (`high-hash`) with Korsh's parameters.

## Autostart (systemd user units)

the units are in [`contrib/systemd/`](../systemd/); see the comments at the top of each and
[`docs/SYSTEMD.md`](../../docs/SYSTEMD.md).
