# Native Windows private-file regression

The release job `windows-msys2` in `.github/workflows/build-windows-msys2.yml`
runs two mandatory standalone gates before building/collecting release binaries.
Neither `--disable-tests` nor the cross-compiled Wine unit-test route bypasses
these gates. Wine results are **not native NTFS ACL acceptance**.

From native MSYS2 MINGW64 with Qt5Core and a C++20 compiler:

```sh
# Supply an existing disposable directory on local NTFS, not a wallet directory.
bash ci/korsh/test_windows_acl.sh /c/path/to/disposable-parent core
bash ci/korsh/test_windows_acl.sh /c/path/to/disposable-parent privileged
```

The wrapper builds in its own unique child directory, links Advapi32 explicitly,
prints source/executable hashes, supplies the required fixture path, and propagates
compile/test failure. The test changes ACLs only on its own NONSECRET fixtures;
its parent directory is not re-ACL'd. Source distributions include the standalone
source, wrapper, documentation, and the production header through the existing
Qt source list. No application build or Qt GUI session is required.

Dynamic Qt is the wrapper default; put its runtime DLL directory on `PATH`.
For the release toolchain use:

```sh
export PKG_CONFIG_PATH=/mingw64/qt5-static/lib/pkgconfig:/mingw64/lib/pkgconfig
export PKG_CONFIG_SYSTEM_INCLUDE_PATH=/mingw64/include
export ACL_QT_LINKAGE=static
bash ci/korsh/test_windows_acl.sh /c/path/to/disposable-parent core
bash ci/korsh/test_windows_acl.sh /c/path/to/disposable-parent privileged
```

Static mode uses `pkg-config --static` and static MinGW runtime flags. It corrects
the Qt 5.15.19 MSYS2 package’s extensionless `-l:libz`/`-l:libzstd` entries to
`-lz`/`-lzstd`; `-static` still requires their real static archives. The test
has its own `main`; do not add it to the existing Qt test executable or invoke it
through argument-less Automake `TESTS`.

## Explicit coverage/privilege boundaries

* **core** needs no elevated fixture privileges. It removes backup, restore, and
  take-ownership privileges from its own token and verifies none remain enabled.
  It checks owner/exact ACE flags, synthetic oracle-negative descriptors, new and
  no-op configs, imported default-owner no-op, replacement, hard links, denial,
  impersonation refusal, ADS refusal, deterministic production Stage lifetime,
  and concurrent exclusive journal publication. It explicitly reports that
  privileged fixtures were not requested. Running core from an administrator
  login does not establish that a separate standard-user login was exercised.
* **privileged** includes core plus real file/directory symbolic links and foreign
  ownership. It requires a token with symlink and restore privileges. The test
  enables those privileges only inside its process; missing privileges or failed
  fixtures **fail**, never skip. Do not change account policy or run the wallet
  elevated to satisfy this test. CI's hosted administrator token supplies the
  fixture capabilities; an unavailable capability blocks release.

The independent oracle requires TokenUser ownership for new/staged/published
objects and one protected, non-inheritable, exact full-access TokenUser allow ACE.
Only the explicitly imported no-op fixture permits a preexisting Administrators
owner when it is the token's default owner. Synthetic owner and ACE-flag negative
fixtures must fail that same oracle. Stage security is checked while the actual
production `WindowsPrivate::Stage` is alive; no scheduler-dependent transient
observation is required. Concurrent publication still requires exactly one
complete winner and preservation against later publishers.

These gates do not establish visible GUI acceptance, crash/power-loss durability,
non-NTFS support, or absence of hostile namespace races. Preserve exact source
hashes with native test logs; rerun after production header changes.
