# Korsh Core v0.0.6.1 — Validation Record

**Validation date:** October 10, 2026

**Release state at validation:** candidate artifacts; no production wallet was opened.

## Source and safety scope

- Source snapshot originated from base commit `28c1635a04574355d5e604873568c0b3c230ceb9`; exact source snapshot SHA-256 `74102ef6d6634f24860bdf19f97f5aa92bb5a2da14076b1118b32aef3aa54f12`; manifest SHA-256 `648b9cbaf1e88fec0dc694b9ed1286024e1e0d73c7f551cd1efd329539f76555` (4,461 files).
- No consensus, genesis, chain-parameter, activation-height, or network-parameter paths were changed (`git diff` audit: 0 such paths).
- All node/wallet smokes used disposable regtest datadirs. The real Mini PC wallet directory was not used. Temporary test data containing generated regtest keys was removed after receipts/logs were retained.
- v0.0.6.1 is a shutdown/recovery-safety hotfix, not a Berkeley DB recovery utility. The prior wallet incident's initiating cause remains unproven; preserve the original wallet and all its Berkeley DB logs if it reports `DB_RUNRECOVERY`.

## Exact release artifacts

The packaged `Release-Notes.md` in all three archives matches SHA-256 `047001f4bada34be25239306fc4ee6db86544c999bdc99275b9e848eac093c0a`. Package receipts and smoke receipts independently recorded the same archive SHA-256 as the files below; the separate `SHA256SUMS.txt` was checked with `shasum -a 256 -c`.

| Platform | Runtime Berkeley DB | Artifact size | SHA-256 |
|---|---:|---:|---|
| Linux x86_64 | 5.3 | 187,344,470 bytes | `2b6d11b7b67ce0d9202c860cf8a079bc3c18a8bdd64040c8c4c4e95f93ef10ae` |
| macOS arm64 | 4.8.30 | 182,416,757 bytes | `ce9d471ed0a0d3c684a5eb0e698d1aa9762765b357732821ef488717698b0682` |
| Windows x64 | 6.2.32, statically linked | 207,108,571 bytes | `69ea0e5e65d3d94b0e12545b8e3e20ce37660613091278a3a0631ea6fd1b7d49` |

## Platform validation

### Windows x64 — physical Mini PC, SSH only

- MSYS2 MINGW64, GCC 16.1.0, static Qt 5.15.19, and static Berkeley DB 6.2.32.
- Full static build and `make check` passed (exit 0). Wallet/DB/util suites and `bdb_rewrite_respects_configured_data_dir_path_budget` passed.
- The exact final ZIP has 12 entries, contains the six expected executables and release documentation, contains no DLL payload, has no non-system PE imports, and none of its executables imports a Berkeley DB DLL.
- Smoke against the exact ZIP: version `0.0.6.1`; zero peers; height 101 after funding and 102 after transfer; one confirmation; wallet transaction count `103 → 103` across restart; address persisted; two clean shutdowns; runtime Berkeley DB 6.2.32; `real_wallet_touched=false`.
- Wizard coverage: the scripted Qt flow with mocked RPC reported 53 PASS, 1 SKIP, 0 FAIL. A separate in-process regtest Qt run also passed `WalletTests::walletTests` (4 passed, 0 failed): it prepared and tested a 1,500 KSH registration, exercised the explicit fee-confirmation step, broadcast only on the disposable regtest, and verified that a valid operator key was saved to the temporary `korsh.conf`.
- The in-process run used a temporary test-driver overlay (SHA-256 `83bd1f0e77ec402dcdaf436e9ae3e302d306a75e2d82a892eb2ac7bdcd5b81a1`) to click the wizard's explicit **Yes** confirmation and match the production success text. The original staged test helper had accepted that dialog with its default **No** and expected a different success string; that caused two harness false-negatives. The repository's staged test file was left untouched and was not included in the release. The test receipt confirms source, test object, and test binary restoration, and the product Qt binary hash remained unchanged (`b038d60ca48b25495c49cf53a0ee1ed9a1d4d0c6716bce518a646b332c0d7da5`).
- No RDP or visible-desktop acceptance was performed; SSH-driven Qt automation is not represented as visual GUI verification.

### Linux x86_64 — WSL2 Ubuntu 22.04

- Build, wallet/DB/util tests, and `make check` passed; runtime Berkeley DB was 5.3 (`libdb_cxx-5.3.so`).
- Smoke against the exact final tarball: zero peers; height 101 after funding and 102 after transfer; one confirmation; transaction count `103 → 103`; address persisted; two clean shutdowns; disposable wallet only.

### macOS arm64

- The v0.0.6.1 build and `make check` passed; the final archive smoke confirmed Berkeley DB 4.8.30 (wallet B-tree version 9), zero peers, transaction/address persistence, and two clean shutdowns.
- The macOS binaries are dynamically linked to Homebrew libraries. Install the dependencies listed in the archive's `README.txt` (`qt@5`, `gmp`, `miniupnpc`, `libnatpmp`, `libevent`, and `libsodium`) before launch; this archive is not a self-contained Qt bundle.
- **GUI caveat:** the exact final archive smoke used `korshd`/`korsh-cli`; it did not launch the GUI (`visible_gui_tested=false`). A separate `test_korsh-qt` test executable aborted on October 9, 2026 with SIGABRT. The crash report matches a test-only executable (UUID `955c903c-5c5a-3756-9af6-c15a6a951120`, SHA-256 `c1ad47b3c1f89615b7a04865fb60ae6297ac3353f614244ed7e1e8907f490e54`); its faulting stack is in QtTest's `QMessageLogger::fatal` on a Qt thread and contains no frame from the shipped `korsh-qt`. The shipped GUI has a different UUID and SHA-256 `cda46e9341017015ed75d673f5176f00bffce6b20a667a53ab0c8d14044230a5`. The abort's root cause and visible Cocoa GUI behavior remain unverified; the abort is not counted as a product-GUI pass or as a demonstrated crash in the shipped executable.
- The final `make check` Qt run used Qt's `minimal` platform: AppTests, WalletTests' GUI body, and AddressBookTests' GUI body reported skips in their logs. Their registrations are not counted as GUI coverage.

## Acceptance limits

- The Windows in-process wizard test is real regtest/RPC execution, not a mock-RPC flow, but it is automated and invisible over SSH; it is not desktop rendering acceptance or mainnet registration.
- Linux and macOS package smokes validate the exact archive's daemon/wallet persistence and clean shutdown, not visible Qt behavior.
- The release preserves each platform's v0.0.6 Berkeley DB backend; wallet files are not asserted portable between platforms or backend versions.
