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
