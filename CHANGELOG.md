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
