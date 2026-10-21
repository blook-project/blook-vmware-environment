# `bvmenv e2e` contract (`bvmenv.e2e.v1`)

## Guest-only guarantee (read this first)

`bvmenv` is a **hypervisor driver, not a test runner on the host**. Every
`exec`, `push`, `pull` and `e2e` operation happens through VMware guest
operations (`vmrun runProgramInGuest` / `copyFileFromGuestToHost`) inside a
`bvmenv`-managed VM. There is no host execution mode, no "run on host" flag and
no fallback path: if the guest is not reachable, the command fails.

Consequences for callers:

* Kernel code (drivers, hypervisors, DSE/physical-memory experiments) may only
  be loaded inside a `bvmenv create`d VM. The lab exists to keep host crash
  experiments off the host. A host-mode helper must never be added.
* `bvmenv exec <vm> -- <cmd>` runs as the guest's `blook` account (local
  administrator, UAC disabled in the base image). It runs with a full admin
  token inside the guest and with **no** access to the host.
* If a payload crashes the guest, the host is unaffected; `state` classifies it
  and `reset` recovers.
* Execution model: `exec` runs the command as the guest's administrator user
  when an interactive session exists, and otherwise (logon/lock UI, hung shell)
  transparently runs the same command line through a **SYSTEM scheduled task**.
  Both paths are elevated inside the guest; `exit_code`, `stdout`, `stderr` and
  `timed_out` are reported identically. A timeout is never retried.
  `bvmenv exec <vm> --self-test` asserts the whole round trip
  (mkdir → copy → run → stdout → exit code → timeout) and is the fastest way to
  check that a VM is usable.

## Host disk safety

`bvmenv` attaches/detaches only its own VHD images and refuses everything else:
the allowlist (`is_owned_vhd_path`) accepts a path only when it is inside
`BVMENV_VM_ROOT` and named `blook-base.vhd`/`answer.vhd`, and disk operations are
always `select vdisk file="<explicit path>"` + `detach vdisk noerr` — never
`select disk N` and never an enumeration of host disks. `bvmenv doctor` reports
any attached file-backed virtual disk (`attached_vhds`) read-only. `bvmenv base
build` refuses to start when drive `V:` is already in use rather than cleaning
up an unknown volume.

Stable interface used by `blook-kdu`, `blook-drv`, `blook-drv-lite` and
`blook-kchannel` to run a test payload inside a lab VM and get machine readable
results back. Treat this file as the contract: fields are only ever added, and
an existing field never changes meaning within `bvmenv.e2e.v1`.

## Invocation

```powershell
bvmenv e2e <vm> --bundle <local-dir> --entry <exe|cmd> [--args A] [--timeout N] \
       [--out <dir>] [--reset-on failure|always|never] [--wait-timeout N] [--json]
```

| Option | Meaning |
| --- | --- |
| `<vm>` | VM name created with `bvmenv create` (e.g. `vm-drv`). It is started and waited for when not running. |
| `--bundle <dir>` | Host directory pushed into `C:\blook\e2e\<run_id>\bundle` in the guest (recursive). May be empty for `--entry` pointing at an absolute guest path. |
| `--entry` | Either a bare file name (resolved inside the bundle directory) or an absolute guest path/command. |
| `--args` | Repeatable argument, appended in order. Arguments that contain spaces are quoted for `cmd.exe`. |
| `--timeout` | Wall clock limit for the guest command, seconds (default 300). On expiry the command is killed and `outcome` is `timeout`. |
| `--out <dir>` | Host artifact root (default `<vm_root>\evidence\e2e`). Each run gets `<out>\<run_id>`. |
| `--reset-on` | `failure` (default) resets unless the outcome is `pass`, `always`, `never`. |
| `--wait-timeout` | How long to wait for VMware Tools + guest ops before giving up (default 180 s). |

`run_id` is a UTC stamp, `YYYYMMDDTHHMMSSZ`.

Exit code: `0` when `outcome == "pass"`, `1` otherwise, `2` for usage errors.
The JSON result is always printed to stdout, even on failure.

## Artifact layout

```
<out>/<run_id>/
  result.json              # bvmenv.e2e.v1 (below)
  exec.json                # bvmenv.exec.v1: raw execution result
  stdout.txt               # guest stdout (bytes as produced)
  stderr.txt               # guest stderr
  state.json               # bvmenv.state.v1 sampled AFTER the run
  state-before.json        # bvmenv.state.v1 sampled BEFORE the run
  screen.png               # host-captured frame after the run (BSOD evidence)
  vmware.log.tail.json     # {"issues": [...], "tail": "<last 16 KiB of vmware.log>"}
```

The artifacts are written even when the outcome is `bsod`, `hang` or
`timeout` — they are the root-cause material.

## `result.json`

```json
{
  "schema": "bvmenv.e2e.v1",
  "run_id": "20261021T031500Z",
  "vm": "vm-drv",
  "bundle": "D:\\blook-kernel-project\\blook-drv\\out\\e2e-bundle",
  "entry": "C:\\blook\\e2e\\20261021T031500Z\\bundle\\demo.exe",
  "args": ["--scenario", "hook"],
  "started_at": "2026-10-21T03:15:00Z",
  "finished_at": "2026-10-21T03:15:42Z",
  "duration_ms": 42137,
  "outcome": "pass|fail|timeout|bsod|hang|error",
  "exit_code": 0,
  "timed_out": false,
  "artifact_dir": "C:\\blook-vms\\evidence\\e2e\\20261021T031500Z",
  "exec": { "...": "bvmenv.exec.v1" },
  "reset": { "performed": true, "ok": true, "reason": "e2e:fail",
             "strategy": "snapshot", "duration_ms": 8100 },
  "state_before": { "...": "bvmenv.state.v1" },
  "state_after": { "...": "bvmenv.state.v1" },
  "error": "only present for outcome=error"
}
```

### Outcome rules (evaluated in this order)

1. `exec.timed_out` → `timeout`
2. `state_after.screen.classification == "bsod"` or a non-empty
   `state_after.screen.stop_code` → `bsod`
3. `state_after.hang.suspected` or `state_after.screen.classification == "hang"`
   → `hang`
4. `exit_code == 0` → `pass`
5. otherwise → `fail`

`error` is reserved for harness problems (VM missing, guest unreachable,
bundle push failed); the guest never influences it.

## `bvmenv.exec.v1`

```json
{
  "ok": true, "ran": true, "timed_out": false, "exit_code": 0,
  "run_id": "...", "guest_dir": "C:\\blook\\runs\\...",
  "stdout_path": "...", "stderr_path": "...",
  "duration_ms": 1234,
  "stdout_tail": "last 8 KiB of stdout",
  "stderr_tail": "last 8 KiB of stderr"
}
```

`stdout_path`/`stderr_path` are the host copies pulled out of the guest (empty
when the run timed out before the pull).

## Failure handling rules for callers

* A `bsod`/`hang` outcome means the VM was reset (default `--reset-on failure`),
  so artifacts are the only evidence: keep `screen.png` and `result.json`.
* Do not assume the VM is usable after a non-`pass` outcome without checking
  `reset.ok`.
* Always pass `--timeout`; a hung guest cannot be interrupted from inside, only
  from the host.
