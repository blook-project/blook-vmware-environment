# Base image

`bvmenv base build` produces one pristine Windows 11 VM, powered off, with a
snapshot named `base`. Every fork is a **linked clone** of that snapshot.

## What it does

1. **Refuse to run twice** unless `--force` (then the old image is deleted).
2. **Partition + format on the host** — a 80 GB expandable VHD is created with
   `diskpart`, one active MBR/NTFS partition, mounted as `V:`.
3. **Apply `install.wim` with DISM** — index 4 = Windows 11 Pro on the zh-CN
   consumer media. This replaces the interactive Windows Setup UI.
4. **Install boot files** — `bcdboot V:\Windows /s V: /f BIOS` (MBR/BIOS boot).
5. **Stage the guest payload** — `guest/blook/*` → `V:\blook\`,
   `guest/unattend.xml` → `V:\Windows\Panther\Unattend\Unattend.xml` **and**
   `V:\blook\unattend.xml`, plus `V:\Windows\Setup\Scripts\SetupComplete.cmd` as
   an insurance kick for the provisioning task.
6. **Hint the remaining passes** through the offline registry
   (`HKLM\SYSTEM\Setup\UnattendFile`).
7. **Convert to VMDK** (`vmware-vdiskmanager -r … -t 0`) — VHD cannot hold the
   snapshots / linked clones the lab needs.
8. **Write the `.vmx`**, then boot: Windows runs the `specialize` and
   `oobeSystem` passes from the answer file, autologons as `blook`, and the
   provisioning task configures the machine.
9. **Wait for the guest to power itself off**, then snapshot `base`.

## Why not the normal unattended-install flow

Two blockers were measured on this host (Workstation 25.0.0.24995812, Windows 11
25H2 zh-CN consumer media) and are recorded here so nobody re-discovers them:

* **VMware's EFI firmware cannot boot the install media from a SATA CD-ROM.**
  `vmware.log` reports `Guest: Status upon boot failure: Time out` and then
  `No operating system was found`. Setting `firmware = "bios"` boots the same
  ISO immediately.
* **Windows Setup does not read `autounattend.xml` from auxiliary media** on this
  media: a second CD-ROM, a FAT12 floppy image (verified, with long file names)
  and a small VHD hard disk all failed to drive the install — Setup stayed
  interactive ("输入产品密钥"). Remastering the ISO was not an option either:
  `install.wim` is 7.4 GB and the bundled `mkisofs` refuses files > 4 GiB and
  cannot be told to allow them; `oscdimg` is not installed.

The DISM path is deterministic, faster (no OOBE screens), and works on any host
with the WDK/SDK tooling that is already required. The answer file is only used
for the passes that actually run — `specialize` and `oobeSystem`.

## Guest provisioning (`guest/blook/provision.ps1`)

Runs as `SYSTEM` from a boot-triggered scheduled task (`C:\blook\provision.cmd`),
two phases, idempotent, everything logged to `C:\blook\provision.log`:

**Phase 1** (first boot after `specialize`)

* waits for `explorer.exe` (OOBE done), continues anyway after 25 min,
* disables UAC / first-logon animation / lock screen / inactivity lock,
* installs **VMware Tools** silently from the Tools ISO (found by scanning
  drives for `manifest.txt` + `VMwareToolsUpgrader.exe`),
* disables VBS / HVCI / Credential Guard and `hypervisorlaunchtype` (a nested
  hypervisor needs VT-x for itself),
* `bcdedit`: `testsigning on`, `nointegritychecks on`, `debug on`,
  `dbgsettings serial debugport:1 baudrate:115200`,
  `bootstatuspolicy ignoreallfailures`, `recoveryenabled No`,
* bugcheck behaviour: `AutoReboot=0`, `DisplayParameters=1`,
  `CrashDumpEnabled=0` so a BSOD **stays on screen** and is OCR-able,
* `i8042prt\Parameters\CrashOnCtrlScroll=1` (Ctrl+ScrollLock bugcheck primitive),
* power: hibernate off, fast startup off, no sleep/display/disk timeouts,
  high-performance plan,
* Windows Update disabled (policy + services), Defender realtime disabled with
  exclusions, telemetry/search/sysmain disabled, system restore off,
* then reboots.

**Phase 2** (second boot)

* writes **persistent autologon** to
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`
  (`AutoAdminLogon`, `DefaultUserName/password`, `ForceAutoLogon`); the
  unattend `LogonCount` only covers the first logons, and a fork that lands on
  the logon screen cannot be used for guest operations at all,
* re-applies `bcdedit /set {current} hypervisorlaunchtype off` and records the
  raw `bcdedit /enum` lines plus `Win32_ComputerSystem.HypervisorPresent` in
  `provision.json`,
* loads the ring 0 **VT-x probe** (`guest-vtx-probe.sys`) and records
  `CPUID.1:ECX.VMX`, `IA32_FEATURE_CONTROL` lock/VMXON-outside-SMX and
  `IA32_VMX_BASIC` in `provision.json.nested_virt`,

* waits for the `VMTools` service, verifies the baseline with `bcdedit`,
* writes `C:\blook\provision.json` (schema `bvmenv.provision.v1`),
* removes the provisioning task, writes `C:\blook\provision.done`, shuts down.

The host then snapshots the powered-off VM as `base`.

## Contract verification

`bvmenv base verify` is host-side and fast by default: it asserts the snapshot
exists, the base is powered off and the disk/vmx are present, and it never boots
anything. `bvmenv base verify --deep` additionally forks a throwaway VM from the
snapshot and runs `guest/blook/verify.ps1` plus `nested-virt-check.exe`, and it
fails hard when `provision.json.failures` is non-empty. The deep checks assert:

* `testsigning on`, `nointegritychecks on`, `debug on`, serial debug settings,
* `hypervisorlaunchtype off`, VBS and HVCI off,
* `AutoReboot=0`, `DisplayParameters=1`, `CrashOnCtrlScroll=1`,
* UAC disabled, fast startup off, Tools running, `blook` account present,
* lock screen disabled,
* `CPUID.1:ECX.VMX == 1` **inside the guest** (nested virtualisation),
* `C:\blook\provision.json` present with the expected schema.

The output is `bvmenv.base-verify.v1` JSON; exit code 0 means the image still
satisfies the contract. The base image is never booted for verification, so its
clean state is preserved.

## Rebuilding / tuning

* `bvmenv base build --iso <path>` uses different installation media (the
  image index must be Windows 11 Pro; adjust `DiskConfiguration`/`/Index:4` in
  `lab.cc` for other editions).
* `--disk-gb N` sizes the base disk (default 80).
* Base memory/CPU defaults come from `BVMENV_MEMORY_MB` / `BVMENV_CPUS`.
* Tuning knobs that must survive snapshots (test signing, bugcheck behaviour,
  tools) live in `guest/blook/provision.ps1`; after editing, `--force` rebuild.
