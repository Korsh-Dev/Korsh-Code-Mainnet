# Korsh Core v0.0.6.1

This hotfix addresses a Windows wallet shutdown crash after Berkeley DB reports that `wallet.dat` needs recovery.

In one sentence: v0.0.6.1 stops shutdown from turning a Berkeley DB failure into an unhandled exception or a cleanup that discards the wallet's recovery logs.

## Observed failure

The affected wallet's Berkeley DB diagnostics report that `wallet.dat` references a log sequence number beyond the available transaction-log end, followed by failed page flushes and `DB_RUNRECOVERY`. During shutdown, the application then surfaced BDB error `-30972` while opening `wallet.dat`. This confirms a wallet/environment log mismatch as the immediate failure; the event that first left the log sequence incomplete is not established. This hotfix preserves the files and prevents the shutdown exception; it does not reconstruct missing recovery logs.

## What changed

- Removed the fallback that moved Berkeley DB environment logs aside and retried with a fresh environment based on text found in the append-only `db.log`. Wallet verification also no longer treats a historical `unsupported DB version` message as proof that the current wallet file uses a newer format.
- Berkeley DB open, transaction-abort, record, cursor, checkpoint, LSN-reset, and close failures that require recovery are latched as recovery-required; transaction-abort cleanup also fails closed on any nonzero result. Already-open batches stop normal operations after recovery is detected, and rewrite treats all nonzero Berkeley DB return codes as failures. Shutdown preserves the existing wallet and transaction-log files rather than treating the environment as clean.
- Windows rewrite temporaries now budget against every Berkeley DB data directory, including root-relative and nested `DB_CONFIG set_data_dir` paths, while keeping candidate collision checks and DB_EXCL protection intact. Both the wallet-suffixed name and short sibling fallback leave path headroom for Berkeley DB's transactional rename/commit. If no safe collision-resistant name fits, rewrite fails before closing, checkpointing, or replacing the original database file; an encryption operation reports failure rather than claiming a successful rewrite.
- A transient Berkeley environment teardown during automatic wallet backup is reported without setting the process-wide backup count to a permanent failure value; the failed backup is not retried in that call, but later backup attempts and CoinJoin remain enabled.
- Automatic Windows Berkeley DB file conversion now serializes the full close/reset/convert/reopen sequence against other database opens and refuses conversion while a database batch is active, preventing replacement of an environment handle while another wallet operation can use it.
- The wallet-directory lock now remains held across conversion, environment reload, and shutdown log cleanup. If conversion or shutdown enters recovery-required state, the lock is retained rather than exposing the wallet directory to a second process during uncertain cleanup.
- Recovery-required state is now sticky for a wallet directory for the lifetime of the process. Unloading and retrying a failed wallet in that same process cannot reopen its recovery files; a fresh process is required before another recovery attempt.
- Berkeley environment, recovery-latch, and directory-lock identities now use weakly canonical paths, so a symlinked wallet-directory alias reuses the same environment and cannot bypass the same-process recovery latch.
- Best-block locator persistence failures are logged and latch the wallet's flush fail-stop. Chain-state flush/close paths surface failures without throwing through the chain callback or continuing later flushes. A rescan's status still describes the block scan itself; if saving its locator fails, the previous locator may cause a later rescan.
- Wallet loading, transaction-hash enumeration, and wallet dumps now fail if an explicit database-cursor close fails. Wallet-load cursor failures return a generic load failure unless a more specific corruption or version error was already detected.

## Important wallet-safety note

This is a shutdown-safety fix, not a repair utility. If a wallet already reports that Berkeley DB requires recovery, keep the original wallet and logs together, make a verified copy, and perform any recovery attempt only on that copy. Do not delete the `database` directory or its log files.

## Compatibility and package

- No consensus, genesis, activation-height, or network-port changes; the legacy wallet data format is unchanged.
- No peer-discovery or connection code changed. Prior connection-refusal diagnostics are outside this wallet hotfix and remain unresolved.
- Each platform retains the Berkeley DB backend used by v0.0.6: Windows 6.2.32, Linux 5.3, and macOS 4.8.30. This does not establish cross-platform wallet portability.
- macOS bundle metadata uses Apple-compatible version fields: the short version is `0.0.6` and the build version is `6.1`, while Korsh's full application/package version remains `0.0.6.1`. Windows installer/resource metadata uses all four numeric components (`0.0.6.1`).
- The Windows ZIP places six executables under `bin/`, setup guides under `docs/`, and Sapling parameter files under `params/`. Windows Qt and Berkeley DB 6.2.32 are statically linked; keep the complete archive together and do not extract only an executable.

## Validation scope

All node and wallet smoke tests use disposable data; no production wallet was opened or modified. No consensus, genesis, activation, or network-parameter files changed.

- **Windows x64:** MSYS2 MINGW64 / GCC 16.1.0, static Qt 5.15.19, static Berkeley DB 6.2.32. The complete static build and `make check` passed (exit 0); wallet/DB/util suites and the configured-data-directory rewrite regression passed.
- **Linux x86_64:** WSL2 Ubuntu 22.04, Berkeley DB 5.3. Build, wallet/DB/util suites, and `make check` passed. The tested package smoke verified two clean shutdowns, zero peers, and wallet transaction/address persistence across restart.
- **macOS arm64:** Berkeley DB 4.8.30. The v0.0.6.1 build and `make check` passed.
- **Masternode wizard:** Windows Qt scripted-flow harness reported 53 PASS, 1 SKIP, 0 FAIL using mocked RPC. This exercises the wizard flow but is not visual desktop acceptance and does not submit a mainnet registration.
- Exact release archive SHA-256 values and extracted-package smoke results are listed in the accompanying `VALIDATION.md`.
