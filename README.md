# kde_mount — network mounts via generated systemd units

The repository is `kde_mount`; the software it builds is **nasmount** — the
`nasmount-*` binaries, the `kcm_nasmount` System Settings module and the KAuth
helper `io.github.pakru.nasmount`.

It mounts CIFS/SMB shares as real kernel mounts at **any path you choose**, using
generated static systemd `.mount`/`.automount` pairs. A share mounts **on demand**
— the first time a process opens its path — and the idle timeout releases it
while the trigger stays armed. Session shares keep their password in KWallet and
arm at sign-in; System shares are an opt-in whose root-owned credential survives
reboot and arms at boot, before login.

| Tool | Kernel mount | Arbitrary mount path | Password store |
|------|--------------|----------------------|-----------------|
| Dolphin `smb://` + kio-fuse | no (FUSE) | no | KWallet |
| Smb4K | yes | no — derived from host/share | KWallet |
| this | yes | **yes** | KWallet, or a root-owned `/etc` credential |

Two ways in, one backend: **System Settings → Network Mounts** to list, add and
remove shares with live state for each, or the **Dolphin service menu** —
right-click an `smb://` share → **Mount as Network Drive…**.

## Requirements

A **Plasma 6 / KF6** desktop and **Linux 6.8+** (for `STATX_MNT_ID_UNIQUE`).

| Need | Debian/Ubuntu package |
|------|-----------------------|
| CMake ≥ 3.20, C++20 compiler | `cmake`, `g++` |
| Qt 6 Core / Widgets / Concurrent / Quick / QuickControls2 | `qt6-base-dev`, `qt6-declarative-dev` |
| Extra CMake Modules | `extra-cmake-modules` |
| KF6 Auth / I18n / WidgetsAddons / Config / Wallet / CoreAddons / KCMUtils | `libkf6auth-dev`, `libkf6i18n-dev`, `libkf6widgetsaddons-dev`, `libkf6config-dev`, `libkf6wallet-dev`, `libkf6coreaddons-dev`, `kf6-kcmutils-dev` |
| `mount.cifs` at runtime | `cifs-utils` |

`install.sh` stops before building if any of these is missing.

## Build and install

```bash
make install           # or: ./install.sh — same thing
```

**Do not run this with sudo.** The build runs as you; only `cmake --install`
elevates, and the script refuses to start as root.

The prefix is **`/usr`**, not `/usr/local`: D-Bus only scans
`/usr/share/dbus-1/system-services` and polkit only
`/usr/share/polkit-1/actions`, so a `/usr/local` install would build fine and
then silently fail to authenticate.

This is a **clean install only** — there is no migration from an older version.
If one is installed, run `./uninstall.sh` first.

`make` builds into `build/` without installing; `make clean` removes it. The
shell scripts exist because they gate on the tests passing, refresh Dolphin's
and System Settings' caches, and enable both lifecycle services.

The KAuth policy is **passwordless for active local users** (`allow_active=yes`,
as in Smb4K's mount helper) and assumes the person at the machine is its
administrator — if that does not hold where you deploy this, read the design
document's safety model first.

## Tests

```bash
make test              # or: ctest --test-dir build --output-on-failure
```

Thirteen test binaries plus a script gate; `install.sh` runs every one and
refuses to install if any fail. Privileged accept paths and real reboot /
no-login behaviour must be validated in a disposable VM.

## Uninstall

```bash
./uninstall.sh
```

An authenticated full purge: every managed share, its credentials and runtime
records, the `nasmount` KWallet folder, `~/.config/nasmountrc`, then the software
itself. Mount points are retained; tampered state or an unsafe live mount is
refused, leaving everything installed for retry.

It reads `build/install_manifest.txt` and refuses to run without it, so
**uninstall works only from the build tree that installed** — after `make clean`,
or in a fresh checkout, re-run `./install.sh` first.

## Further reading

- [`docs/credential-modes-design.md`](docs/credential-modes-design.md) — state
  model, safety and authorisation rules, session lifecycle, blocked operations.
- [`AGENTS.md`](AGENTS.md) — architecture, privilege invariants, conventions.
