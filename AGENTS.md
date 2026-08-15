# AGENTS.md — nasmount

A KDE/Plasma 6 tool that mounts CIFS shares by generating **static systemd
`.mount` / `.automount` unit pairs** in `/etc/systemd/system`. Two front ends
(a System Settings KCM and a Dolphin service-menu dialog) drive one privileged
KAuth helper. Qt 6 / KF6, C++20, CMake.

Read [`README.md`](README.md) for what the tool does and
[`docs/credential-modes-design.md`](docs/credential-modes-design.md) for *why*
it is built this way — the state model, authorisation rules, and session
lifecycle. Code comments cite that document by section (`design §6.2`) and the
implementation plans by section (`plan §7.3.2`); keep doing that when you
encode a rule whose reason is not local.

## Build, test, install

```bash
make                  # configure + build into build/ (RelWithDebInfo, /usr prefix)
make test             # build, then ctest --output-on-failure
ctest --test-dir build --output-on-failure   # tests only
build/bin/<name>_test                        # one test binary directly
make install          # runs ./install.sh — NOT with sudo
make uninstall        # runs ./uninstall.sh
make clean            # rm -rf build/
```

- **Never run `install.sh` as root** — it refuses. It builds as you and
  elevates only for `cmake --install`.
- The prefix is **`/usr`**, not `/usr/local`: D-Bus only scans
  `/usr/share/dbus-1/system-services` and polkit only
  `/usr/share/polkit-1/actions`. A `/usr/local` install builds and then
  silently fails to authenticate.
- Installing is disruptive (writes system D-Bus/polkit files, enables two
  systemd services). Don't install unless asked.
- `uninstall.sh` reads `build/install_manifest.txt` and refuses to run without
  it, so **uninstall only works from the build tree that installed** — a clean
  checkout, or one where `make clean` has run, cannot uninstall until
  `install.sh` regenerates the manifest. The manifest content is deterministic
  (validated against the hardcoded allowlist in
  [`cleanupvalidation.cpp`](src/session/cleanupvalidation.cpp#L27), which also
  requires it to be complete), so regenerating it is safe.
- The repo has **no commits** yet. `build/` is the only build directory the
  tooling knows about; `.gitignore` also covers `build-*/` for ad-hoc trees.

## Architecture and the linkage invariant

Three static libraries, and which binaries may link them is a **security
boundary**, not a style preference:

| Library | Contents | Linked into |
|---------|----------|-------------|
| `nasmount-core` | validation, unit-value encoding, read-only state model (`src/core`) | everything, helper included |
| `nasmount-session` | KConfig store, KWallet, per-user lock, KAuth call wrapper, async operation controller, display model (`src/session`) | dialog, KCM, supervisor, cleanup — **never the helper** |
| `nasmount-root` | durable fd-based filesystem ops, root lock, systemd execution, credential/runtime stores (`src/root`) | `nasmount-helper`, `nasmount-boot` **only** |

- Everything is **STATIC** on purpose: the privileged helper must not depend on
  a `.so` an unprivileged user could replace. Don't convert these to shared.
- `nasmount_assert_no_root_link()` in [`CMakeLists.txt`](CMakeLists.txt#L166)
  fails the configure step if `nasmount-root` ever reaches
  `nasmount-session`, the dialog, the supervisor, the cleanup tool, or the KCM.
  Structural placement is the real defence; that check catches accidents.
- The helper ([`src/helper/helper.cpp`](src/helper/helper.cpp)) is deliberately
  thin: caller validation, typed argument decoding, root-lock acquisition,
  dispatch into `nasmount-root`, reply conversion. **Do not add filesystem or
  systemd mutation there** — it belongs in `src/root`.
- Everything in a helper argument map is untrusted. Caller identity comes only
  from `KAuth::HelperSupport::callerUid()`, never from the arguments. Validation
  done in the dialog or KCM is UX feedback and is re-done in the helper.

Binaries: `nasmount-helper` (root, D-Bus activated), `nasmount-boot` (root,
started by `nasmount-boot.service`), `nasmount-dialog` (service menu),
`nasmount-supervisor` (arm/disarm at sign-in/logout), `nasmount-cleanup`
(authenticated uninstall), `kcm_nasmount` (QML KCM, [`src/kcm/ui/`](src/kcm/ui/)).

## Conventions

- Every source file opens with a block comment: what the unit is, then
  `SPDX-License-Identifier: GPL-3.0-or-later`, then the *reasoning* that a
  reader would otherwise have to reconstruct. Headers carry the API contracts as
  `/** ... */` doc comments; `.cpp` files carry implementation reasoning. Match
  this density — it is unusually high and it is intentional.
- KDE/Qt style: 4 spaces, brace on its own line for functions and attached for
  control flow, `const QString &` parameters, `QStringLiteral` for literals,
  namespaces `UnitSpec` / `UnitValue` / `Verify` / `Session` / `Root::*`. There
  is no `.clang-format`; follow the surrounding file.
- Errors are reported through `bool` returns plus a `QString *error`
  out-parameter, not exceptions. There is **no logging framework** — user-facing
  output goes to `QTextStream(stdout/stderr)` in the standalone binaries only.
- No in-place Edit exists anywhere. Changing a share's UNC, mount point,
  credentials, authentication or mode is Delete then Add. Don't reintroduce an
  edit path.
- Mode (Session vs System) for an *existing* definition is always re-derived
  from the validated unit marker via `Verify::inspectDefinition()` — never from
  the Store and never from a caller-supplied flag.
- Every client mutation runs on a worker thread under `Session::UserLock`; every
  privileged mutation runs under `Root::RootLock`. KAuth and KWallet calls are
  unbounded waits and must never touch the GUI thread.

## Tests

`tests/*.cpp` are plain `main()` binaries using a local harness (`static int
passed/failed` plus a `check(label, condition, detail)` helper, `return failed
== 0 ? 0 : 1`) — **not** QTest. Copy the pattern from an existing test.

Adding a test means editing **three** places:

1. `tests/<name>_test.cpp`;
2. `CMakeLists.txt` — `add_executable` + `target_link_libraries` + `add_test`;
3. [`install.sh`](install.sh#L44) — the explicit test-binary list, which gates
   installation. It is a hand-maintained list; a new test not added there is
   silently skipped at install time.

[`tests/removed_api_gates.sh`](tests/removed_api_gates.sh) is a grep-based gate
that fails the suite if deleted APIs return — the transaction/recovery engine,
tombstones/Forget, automatic partial repair, in-place Edit/Replace, pending-
transaction presentation roles. If a build fails there, the fix is to stop using
the forbidden identifier, not to loosen the pattern.

Privileged accept paths and real reboot / no-login behaviour are **not** covered
locally and must be validated in a disposable VM.

## Gotchas

- Unit values are encoded by `UnitValue::encodeUnitValue()`; its rules
  (percent-doubling, backslashes and quotes *not* interpreted outside a leading
  quote) were verified empirically against real systemd, not just
  `systemd-analyze verify`. Don't "fix" the encoder from first principles.
- Mount-point authorization is **lexical and never canonicalizing**;
  `openMountpointNoFollow()` walks only the authorized suffix with
  `O_NOFOLLOW`. Authorizing a canonical path and then operating on the lexical
  one is a symlink bypass.
- Generation and validation of unit bodies share the same fixed-value functions,
  so any functional deviation becomes `Tampered`. If you change generation,
  change the shared function — never the two sides separately.
- The KAuth policy is passwordless (`allow_active=yes`) by design; the threat
  model in the README explains what that rests on. Adding actions means editing
  [`io.github.pakru.nasmount.actions`](io.github.pakru.nasmount.actions).
- Requires Linux 6.8+ (`STATX_MNT_ID_UNIQUE`), Plasma 6 / KF6, `cifs-utils`.
