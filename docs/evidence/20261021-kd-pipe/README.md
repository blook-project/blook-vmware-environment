# KD over named pipe — real capture

`bvmenv create vm-kdtest --kd-pipe` creates a fork whose serial port is
`\\.\pipe\blook-vm-kdtest-kd` (VMware is the pipe server). KD must be attached
**before** the guest initialises its debug transport, which is what
`bvmenv kd-log <vm> --reset` orchestrates: it starts kd, waits 4 s, resets the
VM, and captures kd's text output.

Command used:

```powershell
bvmenv kd-log vm-kdtest --seconds 50 --reset --out kd-boot-capture.log --json
```

Result (`bvmenv.kd.v1`): `connected: true`, 3795 bytes captured. Key lines:

```
Opened \\.\pipe\blook-vm-kdtest-kd
Kernel Debug Target Status: [no_debuggee]; ...
Waiting to reconnect...
Connected to Windows 10 26100 x64 target at (Wed Oct 21 14:19:45.350 2026 (UTC + 8:00)), ptr64 TRUE
```

`kd-attach-before-boot.log` is the same flow with `bvmenv start` (no reset) —
it also connects, which is the other valid ordering (kd first, then boot).

Without `--reset`/pre-boot attach, kd reports `[no_debuggee]`: the guest only
connects its debug transport during boot. This is the behaviour drv needs to
read `DbgPrint` output when a nested hypervisor freezes the guest.
