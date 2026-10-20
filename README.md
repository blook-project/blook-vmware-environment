# blook-vmware-environment

VMware Workstation based infrastructure for the `blook-project` Windows kernel
test lab: one command builds a pristine Windows 11 base image, linked clones are
created in seconds, commands run inside the guest, and **the guest state is
classified from the host** — including a real bugcheck (BSOD) or a hard hang,
when the guest userland is dead.

Used by [`blook-kdu`](../blook-kdu), [`blook-drv`](../blook-drv),
[`blook-drv-lite`](../blook-drv-lite) and [`blook-kchannel`](../blook-kchannel)
as their end-to-end test backend.

```
bvmenv create vm-drv                 # linked clone from the base snapshot (~3 s)
bvmenv exec vm-drv --timeout 600 -- sc create demo type= kernel binPath= ...
bvmenv e2e vm-drv --bundle out\ --entry demo.exe --timeout 300
bvmenv state vm-drv --json           # host-side classification, works on BSOD/hang
bvmenv reset vm-drv                  # revert to the clean snapshot (<20 s)
```

## Contents

| Path | What |
| --- | --- |
| `src/` | host side implementation (core utils, VMware backend, state classification, lab orchestration, CLI) |
| `include/blook/bvmenv/` | public headers (reusable by other tools) |
| `guest/` | guest payload: answer file + provisioning scripts shipped into the image |
| `tests/unit/` | host side unit tests (gtest, no VM needed) |
| `tests/guest/` | in-guest artifacts: nested-virt check, BSOD driver, hard-hang driver |
| `tests/e2e/` | example e2e bundles (BSOD / hang / nested virt) |
| `docs/` | architecture, e2e + state contracts, base image, evidence |
| `scripts/` | build + registry publishing helpers |

## Requirements

* Windows 11 host (tested on build 26100), VMware Workstation 17.5+ (tested on
  25.0.0); `vmrun.exe`, `vmcli.exe` and `vmware-vdiskmanager.exe` are used.
* `VMAuthdService` running (`Get-Service VMAuthdService`).
* Windows 11 x64 installation media (ISO) for `base build`.
* Optional: `tesseract` on `PATH` (or `BVMENV_OCR_COMMAND`) to OCR bugcheck
  stop codes. Without it `state` still classifies blue/black/desktop screens,
  it just cannot read the stop code.
* `base build` needs an elevated shell (diskpart/DISM/bcdboot) and re-execs
  itself through `gsudo` when available.

## Build

```powershell
xmake f -y -m releasedbg
xmake -y
xmake run bvmenv-tests            # 39 unit tests
.\build\windows\x64\releasedbg\bvmenv.exe --help
```

In-guest test drivers need the WDK:

```powershell
xmake f -y --guest_drivers=y
xmake -y guest-bsod-driver
xmake -y guest-hang-driver
```

## Quick start

```powershell
# 1. build the base image (unattended, ~15-20 min, once per ISO)
gsudo -d .\build\windows\x64\releasedbg\bvmenv.exe base build

# 2. check the image contract (test signing, kernel debug, no VBS, VT-x in guest, ...)
.\build\windows\x64\releasedbg\bvmenv.exe base verify

# 3. fork, run something, look at the state, reset
.\build\windows\x64\releasedbg\bvmenv.exe create vm-drv
.\build\windows\x64\releasedbg\bvmenv.exe exec vm-drv -- cmd /c ver
.\build\windows\x64\releasedbg\bvmenv.exe state vm-drv --json
.\build\windows\x64\releasedbg\bvmenv.exe reset vm-drv
```

## Command map

| Command | Purpose |
| --- | --- |
| `base build [--iso P] [--disk-gb N] [--force]` | unattended base image + snapshot `base` |
| `base verify [--json]` | machine-checked base image contract (runs on a throwaway fork) |
| `create <name> [--cpus N] [--memory M]` | linked clone from the `base` snapshot |
| `start/stop/reset/delete <name>` | lifecycle; `reset` reverts to the `ready` snapshot |
| `list [--json]` | VM store inventory |
| `exec <name> [--timeout S] [--elevated] [--workdir D] -- <cmd>` | run in guest, capture stdout/stderr/exit code |
| `push/pull <name> <host> <guest>` | copy files or directories |
| `state <name> [--json] [--samples N] [--no-ocr]` | host-side state + screen classification + hang detection |
| `screenshot <name> <out.png>` | host-side console frame capture |
| `wait-ready <name> [--timeout S]` | wait for VMware Tools + guest ops |
| `e2e <name> --bundle D --entry X [...]` | generic test runner with artifacts + auto reset |
| `key <name> <hid> <mod>` / `crash <name>` | key injection / Ctrl+ScrollLock bugcheck |
| `doctor [--json]` / `selftest` | self diagnosis |

Every command accepts `--json`; see the contracts in
[`docs/state.md`](docs/state.md) and [`docs/e2e.md`](docs/e2e.md).

## Design notes

* **No guest network dependency.** Guest operations use VMware's VMCI
  (`vmrun runProgramInGuest`, `copyFileFromGuestToHost`), so a guest with a
  broken network stack is still testable.
* **Host-side observation only.** Frames come from `vmcli MKS captureScreenshot`
  (the hypervisor's framebuffer), never from the guest, so classification works
  during boot, BSOD and hangs where `vmrun captureScreen` would fail.
* **BIOS/MBR base image.** VMware's EFI firmware cannot boot this install media
  from SATA and Secure Boot must stay off for test signing; see
  [`docs/images.md`](docs/images.md).
* **DISM-applied image instead of the interactive installer.** See
  [`docs/base-image.md`](docs/base-image.md) for why the answer file is consumed
  by the `specialize`/`oobeSystem` passes rather than by Windows Setup.
* **Nested virtualisation** (`vhv.enable = "TRUE"`) is on, and the base image
  verifies `CPUID.1:ECX.VMX` inside the guest, because a downstream project
  runs its own hypervisor in the VM.

## Guest credentials

Development only: user `blook`, password `blook` (local administrator, UAC
disabled, autologon). Never use this image outside the lab.

## Status / evidence

Real evidence (base image build, two forks, exec/push, BSOD, hard hang,
reset recovery, screenshots + JSON) lives under
[`docs/evidence/`](docs/evidence/README.md).

## License

MIT — see [LICENSE](LICENSE).

Only use this project against systems you own or are authorised to test.
