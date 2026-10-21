# Images and firmware

## What the lab uses

| Item | Value |
| --- | --- |
| Installation media | `C:\vm\zh-cn_windows_11_consumer_editions_version_25h2_updated_jan_2026_x64_dvd_fb37d9ae.iso` (override with `BVMENV_WINDOWS_ISO`) |
| Edition | index **4** = Windows 11 Pro (zh-CN consumer media) |
| Disk | 80 GB sparse VMDK, single MBR/NTFS active partition |
| Firmware | **BIOS** (`firmware = "bios"`) |
| Secure Boot | n/a (BIOS); on an EFI variant it must be **off** for test signing |
| Nested virtualisation | `vhv.enable = "TRUE"` |
| VMware Tools | from the Workstation `windows.iso` (Tools 13.x) |
| Guest credentials | `blook` / `blook`, local administrator, UAC disabled |

## Known constraints (measured on Workstation 25.0.0.24995812)

1. **EFI + SATA CD-ROM does not boot the Windows 11 install media.**
   `vmware.log` shows `Guest: About to do EFI boot: EFI VMware Virtual SATA CDROM
   Drive (1.0)` followed by `Status upon boot failure: Time out` and
   `[msg.Backdoor.OsNotFound] No operating system was found`. The same ISO boots
   instantly with `firmware = "bios"`. The lab therefore uses BIOS/MBR.
2. **Windows Setup ignores `autounattend.xml` on auxiliary media** here (second
   CD-ROM, FAT12 floppy with long names, small VHD hard disk all tested). The
   base image is therefore applied with DISM from the host (see
   [`base-image.md`](base-image.md)) and the answer file drives only the
   `specialize` / `oobeSystem` passes.
3. `install.wim` is 7.4 GB; the `mkisofs` bundled with Workstation refuses
   files > 4 GiB, and `oscdimg` is not installed. Remastering the ISO is a dead
   end without the ADK.

## UEFI fallback (not built by default)

If a downstream test needs UEFI-specific behaviour (Secure Boot authentication,
DSE/measured boot, UEFI variable attacks), the lab needs an **EFI variant** of
the base image. The path that works:

1. Create the VM with `firmware = "efi"`, `uefi.secureBoot.enabled = "FALSE"`
   and a GPT disk (ESP + MSR + Windows partition, as in
   `docs/evidence/` for the BIOS layout but with `Type=EFI`, `Type=MSR`,
   `Type=Primary`).
2. Repack the installation ISO with the ADK `oscdimg` and use
   `efi/microsoft/boot/efisys_noprompt.bin` as the El Torito EFI boot entry.
   The plain `efisys.bin` waits for a keypress, which a headless VM cannot give
   (this is also why the BIOS path needs no interaction).
3. Apply `install.wim` with DISM to the Windows partition, then
   `bcdboot W:\Windows /s S: /f UEFI`.
4. Keep `uefi.secureBoot.enabled = "FALSE"`: with Secure Boot on, test signing
   (`bcdedit /set testsigning on`) is rejected and unsigned/test-signed drivers
   cannot load, which defeats the purpose of the lab.

Nothing in `bvmenv` hard-codes BIOS: `lab.cc` writes the firmware key, so a
variant can be introduced as an option (`--firmware efi`) with the steps above.

## Why not import a prebuilt image?

The lab deliberately builds from the vendor ISO so that the base image can be
rebuilt on any host with the same ISO hash, and so that every provisioning step
is reviewable in version control instead of hiding in a binary blob.

## Concurrency budget

Up to **4** guests at once is fine on this host (64 GB RAM, 24 threads,
~6 GB per guest); that is the default budget. Six concurrent guests froze the host on 2026-10-21 (Kernel-Power 41, no
dump): the failure mode is host memory/IO starvation, not a guest bug.

`bvmenv` enforces this: `start`/`create` refuse to bring up a fifth managed VM
unless `--allow-many` is passed, and `bvmenv doctor` reports the running count,
their memory footprint and free host RAM. Stale forks left by a host reboot are
removed with `bvmenv delete <vm> --force` (only when no `vmware-vmx` holds them).

## Host precondition: only one VMM driver

Before any VM test, check that no other hypervisor VMM driver is running:

```powershell
bvmenv doctor        # "no other hypervisor VMM driver running" must be ok
bvmenv host-prep     # stops MuMuVMMDrv / VBox* / Nemu* (falls back to gsudo)
```

On 2026-10-21 the host froze twice (Kernel-Power 41, no dump) because MuMu
emulator's VirtualBox-based `MuMuVMMDrv` was loaded alongside VMware's `vmx86`.
The symptom is VMs losing VMware Tools heartbeats and then the host dying - it is
NOT a VM-count problem. `MuMuVMMDrv` is now `start=demand`; if MuMu is needed
later, `sc config MuMuVMMDrv start= system` brings it back at boot, and
`bvmenv host-prep` stops it again before VMware work.

## Driver signing: DSE on, SPC-signed guests

The guest runs with **Driver Signature Enforcement ON** (`testsigning off`).
Test drivers must therefore carry a real SPC chain, signed with blook-drv's
signer scheme:

```powershell
$env:BVMENV_SIGNER_DIR = 'C:\blook-vms\_signer'   # a copy of blook-drv's signer/
pwsh -File scripts/sign-guest-drivers.ps1 -BuildDir build/windows/x64/releasedbg
# spcsign sign --sha1 --cert XINDA --tsa-time 2013-01-01T00:00:00Z --chain ... <driver.sys>
```

The signer is deliberately **not vendored** in this repository (it belongs to
blook-drv); only the invocation contract is documented. `debug on`,
`nointegritychecks on` and the rest of the kernel-debug setup are unchanged.

## Local accounts on this host are not a second machine

`win_dsh-test1` / `win_dsh-test2` are **local Windows accounts on this same
physical machine**, not separate test machines or VMs. Anything that matters for
results (CPU topology, VMM drivers, storage, RAM, the VMware installation) is
identical and shared; only the user profile and its permissions differ.

## SPC signing: SHA-1 only, trust imports required

Confirmed on this host (Win11 26100 guest, DSE enforced via `testsigning off`):

| Signature | Result |
| --- | --- |
| SPC **SHA-1** + TSA root trusted + chain in CA + leaf in TrustedPublisher | **loads** (SCM start OK) |
| SPC **SHA-256** | rejected (`577` / policy) |
| test-signed (`testsigning on`) | loads, lab only |

Provisioning bakes the imports in (admin): `tsa-root.cer` -> Root,
`verisign-cscs-2010.cer` + `verisign-g5-mcvr.cer` -> CA, and the signer
certificate exported from a signed driver -> TrustedPublisher. Drivers are signed
with `spcsign --sha1 --cert XINDA --tsa-time 2013-01-01T00:00:00Z --chain ...`
(see `scripts/sign-guest-drivers.ps1`; the signer itself lives in blook-drv).

## Signing findings (measured, Win11 26100 guest)

| driver signature | testsigning off + nointegritychecks off (DSE) | testsigning on |
| --- | --- | --- |
| **SPC SHA-1** (spcsign, XINDA + 2013 countersig) with `tsa-root` -> Root, `tsa-leaf` -> TrustedPublisher, VeriSign chain -> CA, XINDA -> TrustedPublisher | **loads** (`sc start` State 4 Running -> `KeBugCheckEx` BSOD observed) | loads |
| SPC **SHA-256** | rejected | rejected |
| lab cert SHA-256 (`CN=blook-lab-test`) | rejected `577` | loads (lab fallback) |

The earlier `577` measurements in this repository were **my configuration error**,
not Windows policy: the TSA **leaf** certificate was missing (only the root had
been imported) and a leftover `blook-provision` boot task kept re-enabling
`nointegritychecks` on every boot, so the boot under test was never the boot that
had been configured. With both fixed, SPC SHA-1 is accepted with DSE fully
enforced, matching kchannel's independent evidence.

Trust imports baked into provisioning (admin, LocalMachine):
`tsa-root.cer` -> Root, `tsa-leaf.cer` -> TrustedPublisher,
`verisign-cscs-2010.cer` + `verisign-g5-mcvr.cer` -> CA, `xinda.cer` ->
TrustedPublisher. Drivers are signed with
`spcsign sign --sha1 --cert XINDA --tsa-time 2013-01-01T00:00:00Z --chain ...`
(`scripts/sign-guest-drivers.ps1 -Mode spc`; the signer itself lives in blook-drv,
`BVMENV_SIGNER_DIR`).

GitHub `windows-latest` has `testsigning Yes` by default, so a CI `sc start`
success there is not proof of SCM policy: assert `signtool verify /kp` **and**
record `bcdedit /enum {current}` in the evidence.

## Serial capture: not available on Workstation 25

Both backends were measured and both are **inert**: `serial-probe` shows
`serial.log` stays 0 bytes, and with a named-pipe fork (`create --serial-pipe`,
host as pipe server started before power-on via `serial-log --boot`) the guest
`echo …>COM1` also produces 0 bytes, although the guest has a working COM1
(`mode com1` OK, `Win32_SerialPort` lists COM1). Guest COM1 writes do not reach
VMware's serial device on this build, so serial breadcrumbs are closed as a
route; use `bvmenv dump pull` (below) for kd-free evidence.

## Kernel dumps (kd-free evidence path)

Provisioning configures dumps deterministically: `CrashDumpEnabled=1`,
`DumpFile=C:\Windows\MEMORY.DMP`, `MinidumpDir`, `Overwrite=1`,
`AlwaysKeepMemoryDump=1`, **plus the two things without which nothing is
written**: a page file (`PagingFiles = C:\pagefile.sys 4096 16384`) and a
pre-created dedicated dump file (`DedicatedDumpFile = C:\dedicateddump.sys`,
8 GB, `DumpFileSize = 8192`). Without those, Windows silently produces no
`MEMORY.DMP` - measured: the same bugcheck wrote nothing until both existed, then
produced a **6.4 GB** `MEMORY.DMP`.

Workflow:

```powershell
bvmenv crash <vm>                       # 0xE2, or a test driver that bugchecks
bvmenv state <vm> --json                # bsod (frame) or hang (VIX dead) - both are verdicts
bvmenv start <vm>                       # ONE reboot: the dump is assembled at boot
bvmenv dump pull <vm> --out vm.dmp --json
kd -z vm.dmp -c "!analyze -v; ~* kv; q" # offline analysis, no kd attached to the VM
```

Notes: the copy has a one-hour budget (a kernel dump is multi-GB); `dump pull`
reports the copy error in JSON; `verify.ps1` ("crash dump configured") and
`doctor.crash_dump` guard against regressions.

## Hang vs unknown in `state`

A powered-on guest that stops answering is a **hang**, never `unknown`:

* probes exhaust their budget (VIX dead, no bugcheck frame obtainable) -> `hang`,
  reason "guest did not answer VIX probes ... (wedged: tools heartbeat lost)";
* tools stopped and the frame is unclassifiable -> `hang`;
* three unchanged frames with a dead guest -> `hang` (classic path).

`state --budget <seconds>` controls the per-sample budget (default 20 s).
Measured on a genuinely wedged guest: `classification: "hang"` before the reboot
that assembled its 6.4 GB dump.

## Freeze history and the 1-VM rule (2026-10-21/22)

Six host freezes on this machine, all Kernel-Power 41 with no dump and no WHEA:

| time | context | mitigation afterwards |
| --- | --- | --- |
| 03:36 | host-side kernel load experiments | host kernel loads banned (guest-only rule) |
| 11:30-11:38 | host-side kernel load experiments (x3) | same ban; `_detach-vhd` disk-safety allowlist |
| 15:45 | 6 VMware guests, kd-probe fork up | VM budget guard introduced |
| 17:37 | drv's kd-pipe fork active | KD-over-pipe investigated |
| 17:55 | kchannel's kd window | `kd-pipe` disabled by default (`--unsafe-kd`) |
| 01:09 | 3 guests, MuMu VMM driver stopped, **no kd** | kernel experiments serialized to 1 VM |

MuMu's VirtualBox-based VMM driver was stopped and set to demand start (it was a
real contributor while loaded); KD pipes were banned; the VM budget was added.
The 01:09 freeze had none of those factors, which leaves concurrent guest-kernel
experiments as the standing suspect on Workstation 25 - hence the rule:

**Run one kernel-experiment VM at a time.** `bvmenv doctor` reports
`recommendations` (with a message naming the running VMs) whenever more than one
managed VM is up, and the concurrency guard remains the hard enforcement point.
After any host restart, re-check driver services/files and test-signing residue
before continuing.
