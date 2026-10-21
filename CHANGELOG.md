# Changelog

All notable changes to `blook-vmware-environment` are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [0.1.14] - 2026-10-21

### Changed - KD is disabled by default (host safety)

- **`kd-pipe` / `create --kd-pipe` now refuse unless `--unsafe-kd` is passed.** A
  KD pipe on a VMware guest correlates with every host freeze we have seen
  (15:45, 17:37, 17:54 - Kernel-Power 41, no dump). With the pipe in server mode
  (VMware = server, kd = client) a slow or absent client can block a vCPU; the
  documented client mode (`serial0.pipe.endPoint = "client"`, kd creates the
  server) was implemented and tested here but **did not connect**
  (`Waiting for pipe ... Waiting to reconnect`, 0 bytes captured). Until that is
  understood, kd is opt-in only.
- `kd-pipe --off` still works without the flag (disabling is always safe).
- `doctor` reports `kd_pipe_configured` (managed VMs whose serial0 is a pipe) and
  `kd_pipe_running`, so a leftover pipe is visible before a VM is started.
- Recommended alternative for hung-guest evidence: breadcrumbs (`bvmenv crash`
  to force 0xE2, `bvmenv state` for host-side classification, guest log files).

## [0.1.13] - 2026-10-21

### Added

- **Host precondition check** (root cause of the 2026-10-21 host freeze): `doctor`
  now scans for other hypervisor VMM drivers (`MuMuVMMDrv`, `VBox*`, `Nemu*`) and
  fails the check plus warns when one is RUNNING, with the fix in the message.
  Two type-2 VMMs at once (MuMu's VirtualBox-based `MuMuVMMDrv` alongside
  VMware's `vmx86`) is what killed tool heartbeats and froze the host twice
  (Kernel-Power 41, no dump) - not VM count.
- `bvmenv host-prep [--json]` stops those drivers (`sc stop`, falling back to
  `gsudo -d sc stop`), and reports what it did (`bvmenv.host-prep.v1`). Run it
  before VM tests; MuMu stays available later via `sc config MuMuVMMDrv start= system`.
- `bvmenv kd-stacks <vm> [--commands "..."] [--break-after N]`: attach and
  continue for N seconds (default 30), then reconnect and break in to dump every
  thread (`!thread; ~* kv; q` by default) - the post-hang evidence path.

### Fixed

- `guest_mkdirs` now uses VIX `createDirectoryInGuest` (a guest-file operation
  that needs credentials, not a user session). Push/bundle staging therefore
  works on a guest parked at the logon/lock UI **without** the 0.1.9 recursion.
- `bvmenv --version` prefers the `version.txt` shipped next to the binary (or in
  the repo root), so local builds no longer report the compile-time default
  `0.1.0` while the tree is at a newer version.

## [0.1.12] - 2026-10-21

### Fixed

- **Regression in 0.1.9–0.1.11: `exec`/`e2e`/`wait-ready` crashed with a stack
  overflow.** `guest_mkdirs` was routed through `run()` (added in 0.1.9 to make
  bundle staging work without a user session), but `run_once()` calls
  `guest_mkdirs()` to create its own job directory - so every guest operation
  recursed until the stack overflowed (`0xC00000FD`). `guest_mkdirs` is back on
  the plain script path; the push-at-the-logon-UI limitation is therefore still
  open (documented in docs/e2e.md) and will be solved without recursion.

  **Use 0.1.8 or 0.1.12; do not use 0.1.9–0.1.11 for guest operations.**

## [0.1.11] - 2026-10-21

### Added

- `scripts/validate-e2e-outcomes.ps1` — one-shot, self-cleaning validation of the
  `bvmenv.e2e.v1` failure contract on a real fork: a command that ignores its
  budget must classify as `timeout`, a real bugcheck as `bsod`, a real hard hang
  as `hang`, and the crash cases must have auto-reset the VM
  (`reset.performed: true`). Writes `summary.json` + run artifacts to
  `docs/evidence/e2e-outcomes/`.

### Documentation

- `bvmenv --help` now documents the commands added since 0.1.0: `kd-pipe`, `kd`,
  `kd-log` (with `--reset/--no-continue/--break-after/--commands`), `delete
  --force`, `create --kd-pipe`, `exec --env/--self-test`, `base verify --deep`
  (including the extra out-of-band PowerShell comments such as `Version`,
  `OS`, `RAM`, `State`), `--allow-many`, `BVMENV_MAX_RUNNING_VMS`, and the
  600 s default readiness window.

## [0.1.10] - 2026-10-21

### Added

- `exceeds_timeout_budget()` — the timeout rule shared by `exec` and `e2e` and
  covered by five unit tests (within budget, at/over budget, late artifact, zero
  timeout, already-timed-out). The rule was fixed twice by hand; it now cannot
  drift between the two call sites.
- `docs/e2e.md`: "Timeout vs fail (the rule)" and a note that bundle staging
  works without a user session.

## [0.1.9] - 2026-10-21

### Fixed

- The SYSTEM-fallback **success** path of `exec`/`e2e` bypassed the sticky-timeout
  rule, so a command killed at its budget could still be reported `pass` when its
  exit-code artifact arrived late. Verified: `e2e --timeout 5` against a 40 s
  command now reports `outcome: "timeout"`, `timed_out: true`.
- `guest_mkdirs` (used by `push`/`e2e` bundle staging) ran only through the user
  session, so pushes timed out on a guest sitting at the logon/lock UI. It now
  goes through `run()` and therefore the SYSTEM fallback like `exec`.

## [0.1.8] - 2026-10-21

### Changed

- Concurrency budget raised to **4** concurrent managed VMs (64 GB / 24 threads is
  sized for that); `BVMENV_MAX_RUNNING_VMS` and `--allow-many` unchanged. The
  guard is a guard rail, not a diagnosis: the 2026-10-21 freeze had six guests
  (including a short-lived probe fork), and further hangs at four are to be
  investigated as VMware/driver/storage interactions rather than VM count.
- `bvmenv doctor` emits a **warning** (not a refusal) when free host RAM drops
  below 8 GB, alongside `running_vms` / `running_memory_mb` / `free_ram_mb`.

## [0.1.7] - 2026-10-21

### Added

- **Concurrency guard**: `start`/`create` refuse to bring up a managed VM when 3
  are already running, listing them, unless `--allow-many` is passed
  (`BVMENV_MAX_RUNNING_VMS` overrides the budget). Motivated by the host freeze
  of 2026-10-21 with six concurrent guests (Kernel-Power 41, no dump), where no
  host kernel code was running.
- `bvmenv doctor` reports `running_vms`, `running_memory_mb`, `free_ram_mb`,
  `max_running_vms` and a "running managed VMs within budget" check.
- `bvmenv delete <vm> --force` removes a stale fork directory (no `vmware-vmx`
  holds it, e.g. after a host reboot); a live VM keeps its files locked and the
  removal fails loudly.

### Fixed

- `--timeout` classification is now sticky: a guest command that consumed its
  whole budget is reported `timed_out: true` (exit code -1) even when its
  exit-code artifact only arrives after the client window closed. This is what
  made `e2e --timeout` report `pass` for a command bvmenv had already killed.
- `bvmenv.exec.v1` reports `exec_timeout_s` (0.1.6).

## [0.1.6] - 2026-10-21

### Added

- `bvmenv.exec.v1` reports the **effective guest-command timeout**
  (`exec_timeout_s`), so "did `--timeout` reach the guest?" is answerable from
  `result.json` / `exec.json` instead of requiring a repro. Verified: e2e with
  `--timeout 5` shows `exec_timeout_s: 5`.

### Known issue (tracked)

- When the guest command ignores its budget, `exec --timeout` correctly reports
  `timed_out: true` (direct runs verified), but in an `e2e` run the same command
  can be reported as `timed_out: false, exit_code: 0` because the SYSTEM
  fallback's exit-code file appears after the wait window (the guest task keeps
  running). `exec_timeout_s` now makes this visible; the classification fix is
  the next item (treat "artifact appeared after the deadline" as a timeout).

## [0.1.5] - 2026-10-21

### Fixed

- `kd-log` on a fork that was not created with `--kd-pipe` (or not converted)
  now fails fast with an actionable message instead of waiting forever on a pipe
  server that never appears: `has no KD pipe (serial0 is not \\.\pipe\blook-<vm>-kd);
  convert it with bvmenv kd-pipe <vm> (VM powered off) or re-create it with
  --kd-pipe` (exit code 2).
- `kd-log --reset` now handles a **powered-off** fork: it starts the VM (which is
  what makes KD catch the boot) instead of resetting nothing; a running fork is
  still reset. Verified: `started: true`, `connected: true`,
  `Connected to Windows 10 26100 x64 target ...`.

## [0.1.4] - 2026-10-21

### Added

- `bvmenv kd-pipe <vm> [--off]` converts an *existing* fork (no re-clone) to
  named-pipe KD: it rewrites `serial0` to `\\.\pipe\blook-<vm>-kd` with VMware as
  the server (or back to `serial.log`), refuses to touch a running VM, and prints
  the exact `kd.exe -k com:pipe,port=...,resets=0,reconnect` command line.
  Verified on a fork created without `--kd-pipe`: `kd-pipe` → `start` →
  `kd-log --reset` reports `connected: true` and captures
  `Connected to Windows 10 26100 x64 target` + `Kernel base = ...`, and the guest
  still answers `exec` afterwards.
- Pipe naming rule documented in the README: `\\.\pipe\blook-<vm>-kd`.

## [0.1.3] - 2026-10-21

### Fixed

- `kd-log` no longer leaves the guest paused at the initial breakpoint: it
  continues the target by default (`kd -c "g"`), captures through kd's own
  `-logo` session file, and terminates kd at the end so the target resumes.
  Verified: `connected/continued/target_resumed = true` and the guest answers
  `exec` afterwards.
- `e2e` default readiness budget raised to 600 s (a freshly booted linked clone
  needs minutes on a shared host) and `result.json` now records a real
  `duration_ms`/`finished_at` (it was written before those were set).

### Added

- `kd-log --no-continue` (attach and stay broken, e.g. to inspect boot) and
  `--break-after N --commands "<cmds>"` (Ctrl+Break break-in followed by
  debugger commands; Ctrl+Break needs a shared console, so headless runs should
  instead keep kd attached and force a bugcheck with `bvmenv crash`).
- Evidence note `docs/evidence/20261021-kd-pipe/continue-by-default.md`.

## [0.1.2] - 2026-10-21

### Added

- `bvmenv kd-log <vm> --reset` attaches kd, resets the VM and captures the boot
  KD stream (KD must be attached before the guest initialises its debug
  transport; attaching later only reports `[no_debuggee]`). `--json` now reports
  `connected` alongside the captured log path.
- Real KD capture evidence under `docs/evidence/20261021-kd-pipe/`
  (`Connected to Windows 10 26100 x64 target ...`).

## [0.1.1] - 2026-10-21

### Added

- `bvmenv create <name> --kd-pipe` rewrites the fork's serial port to a named
  pipe (`\\.\pipe\blook-<vm>-kd`, VMware as server), `bvmenv kd <vm>` prints the
  matching `kd.exe -k com:pipe,port=...` command line, and
  `bvmenv kd-log <vm> --seconds N --out file` runs kd and captures its text
  output (the only usable way to read DbgPrint output).
- `bvmenv exec --env KEY=VALUE` (repeatable) injects guest environment variables
  into the generated job file so the child process inherits them.

### Fixed

- Command dispatch fell through to `crash` for unrelated commands after the kd
  insertion (caught by the CI smoke test); `doctor`/`selftest` work again.
- `state` guest probes are bounded (8-10 s) so it always answers on a hung guest.
- Failed e2e runs in the evidence directory are kept under `e2e-failed/` so the
  evidence set is unambiguous.

## [0.1.0] - 2026-10-21

### Added

- `bvmenv` CLI (C++20, xmake): `base build|verify`, `create`, `start`, `stop`,
  `reset`, `delete`, `list`, `exec`, `push`, `pull`, `state`, `screenshot`,
  `wait-ready`, `e2e`, `key`, `crash`, `doctor`, `selftest`.
- Unattended Windows 11 base image build: host side partitioning (VHD), DISM
  image apply, BIOS boot files, `specialize`/`oobeSystem` answer file, guest
  provisioning (VMware Tools, test signing, kernel debug over serial, bugcheck
  stays on screen, UAC/VBS/HVCI off, no sleep/hibernate, Windows Update and
  Defender realtime disabled), clean shutdown and snapshot `base`.
- Linked clones from the `base` snapshot with a `ready` snapshot for fast
  (`<20 s`) `reset`.
- Guest execution over VMCI with stdout/stderr/exit-code capture, timeouts and
  an `--elevated` (SYSTEM scheduled task) mode.
- Host-side state classification that never depends on guest userland:
  power/tools state, frame capture via `vmcli MKS captureScreenshot`, colour
  heuristic classification (`desktop`/`login`/`bsod`/`boot`/`black`/`hang`),
  optional stop-code OCR, hang detection from unchanged frames, `vmware.log`
  and guest serial log issue extraction; `bvmenv.state.v1` JSON.
- `bvmenv e2e` runner with the stable `bvmenv.e2e.v1` artifact contract
  (`result.json`, `stdout/stderr.txt`, `state.json`, `screen.png`,
  `vmware.log.tail.json`), outcome classification (`pass|fail|timeout|bsod|hang|error`)
  and reset policies.
- Guest test artifacts: `nested-virt-check.exe` (CPUID VMX + firmware
  virtualisation), `bvmenobsod.sys` (KeBugCheckEx on load) and `bvmenohang.sys`
  (HIGH_LEVEL spin) plus `bvmenv crash` (Ctrl+ScrollLock, bugcheck `0xE2`).
- Unit tests for JSON, argument parsing, `.vmx` handling, screen classification,
  stop-code extraction and the JSON report contracts.
- GitHub Actions CI (build + unit tests + CLI smoke test + clang-format check)
  and a tag driven release workflow that publishes a package zip and SHA256.

### Fixed

- Guest execution was broken in three independent ways and is now covered by
  `bvmenv exec <vm> --self-test` (mkdir, copy, run, stdout, exit code, timeout):
  batch-file content was quoted with argv rules (`\"` is not a batch escape),
  the exit code was read with `%ERRORLEVEL%` inside a VIX-hosted batch where it
  expands to nothing (now `setlocal EnableDelayedExpansion` + `!ERRORLEVEL!`),
  and the non-elevated VIX script path needs an interactive session, so guests at
  the logon/lock UI failed outright (now falls back to a SYSTEM task).
- `wait-ready` no longer blocks on a user-session guest call: it probes through
  the same fallback path, so a fresh clone reports ready without auto-logon, and
  it fails fast for unknown or powered-off VMs.
- `base build`: generated `.vmx` had no PCIe root ports and used `vmxnet3`, so
  power-on failed with `[msg.pci.noslotavail]`; now `e1000e` + `pciBridge4..7`,
  and the hypervisor's own reason is surfaced in the error message. A failed
  build no longer leaves a half-built `base` behind.
- Screen classification no longer reports a false BSOD for the Windows logon UI:
  a blue frame with a live guest is `login`; `bsod` requires bugcheck OCR text or
  a dead guest (plus an unchanged frame when sampling).
- Guest test drivers/probes are signed with the lab certificate during
  `base build` (staged into the guest Root/TrustedPublisher stores), so they load
  with `testsigning on`; the VT-x probe now reports its raw
  `IA32_FEATURE_CONTROL` / `IA32_VMX_BASIC` values.
- `base verify` parsing false positives (bcdedit `hypervisorlaunchtype Off`,
  `HypervisorPresent=false`) are gone; `base verify` is host-side by default and
  `--deep` performs the live guest contract and fails on `provision.json.failures`.
- Host disk safety: bvmenv only ever attaches/detaches its own VHDs by explicit
  path inside `BVMENV_VM_ROOT` (allowlist + unit tests); `doctor` reports attached
  file-backed virtual disks read-only, and a failed `base build` on an occupied
  `V:` stops instead of touching unknown volumes.
