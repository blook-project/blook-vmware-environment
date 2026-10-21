# Changelog

All notable changes to `blook-vmware-environment` are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

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
