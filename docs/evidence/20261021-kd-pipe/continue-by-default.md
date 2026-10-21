## Continue-by-default fix (0.1.3)

An earlier `kd-log` attached and left the guest **paused at the initial
breakpoint** (`Break instruction exception 80000003`, prompt `0: kd>`), which made
the VM look hung and polluted downstream experiments.

From 0.1.3 `kd-log` always continues the target and terminates kd at the end
(which resumes the target). Verified on a live fork:

```
$ bvmenv kd-log vm-kdtest --seconds 40 --reset --json
"connected": true, "continued": true, "target_resumed": true, "bytes": 4033

$ bvmenv exec vm-kdtest -- cmd /c echo GUEST-ALIVE-AFTER-KD-CONTINUE
GUEST-ALIVE-AFTER-KD-CONTINUE
```

The capture contains kernel boot text (`Kernel base = 0xfffff801...`,
`System Uptime: ...`, `DriverEntry failed ...` for drivers), i.e. DbgPrint and
the loader's messages stream while the target runs.

Notes:
* `--no-continue` restores attach-and-stay-broken (useful to inspect at boot).
* `--break-after N --commands "<cmds>"` attempts a Ctrl+Break break-in followed
  by debugger commands. Ctrl+Break delivery requires the CLI to share a console
  with kd; in a headless/harness run that is not possible, so the reliable
  headless recipe is: keep kd attached and continuing, then force a bugcheck
  (`bvmenv crash <vm>`, i8042 Ctrl+ScrollLock) so the kernel prints the bugcheck
  and stacks into the capture.
