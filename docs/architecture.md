# Architecture

`blook-vmware-environment` is a thin, host-only control plane over VMware
Workstation. Nothing is installed in the guest except what the base image
provisioning puts there.

```
+---------------------------- bvmenv.exe (host, C++20) ----------------------------+
| cli/        argument parsing, JSON/human output, exit codes                       |
| lab/        orchestration: create / reset / state sampling / e2e / base build     |
| state/      WIC PNG decode -> statistics -> classification; OCR; log parsing      |
| vmware/     vmrun.exe + vmcli.exe backends, .vmx read/write                       |
| core/       process spawn with timeout + job object, JSON, config, locks, logging  |
+---------------------------------------+-------------------------------------------+
                                        | CreateProcess
        +-------------------------------+-------------------------------+
        |                               |                               |
   vmrun.exe                       vmcli.exe                   vmware-vdiskmanager.exe
   power/snapshot/clone            MKS screenshot/keys         VHD -> VMDK
   guest ops over VMCI             Tools/Power query
        |                               |                               |
        +----------------------------- hypervisor ----------------------+
                                        |
                              +---------+---------+
                              |   Windows guest   |
                              |  (base snapshot)  |
                              +-------------------+
```

## Layers

### `core/`

* `process.cc` — `CreateProcessW` with piped stdout/stderr, a Job Object so a
  timeout kills the **whole** tree (no orphaned `vmrun.exe`), UTF-8/UTF-16
  helpers, `build_command_line` with correct Windows quoting.
* `json.cc` — small JSON value/writer/parser. Reports and tests rely on it; it
  intentionally has no third-party dependency.
* `util.cc` — filesystem/string helpers, a file logger (`--verbose`), and
  `FileLock` (named mutex) so concurrent invocations cannot corrupt the store.
* `config.cc` — resolves VMware's install directory from the registry, the VM
  store (`BVMENV_VM_ROOT`, default `C:\blook-vms`), guest credentials, OCR
  command and tunables.

### `vmware/`

* `vmx.cc` — `.vmx` document model: order preserving, unknown keys kept, values
  quoted on save. Used to fix up linked clones (identity, MAC, serial log).
* `vmware.cc` — the backend. Every operation is a subprocess; no VIX COM
  dependency, so behaviour is easy to reproduce from a shell.
  * power/snapshot/clone/delete through `vmrun -T ws`.
  * host-side observation through `vmcli` (`MKS captureScreenshot`,
    `MKS sendKeyEvent`, `Tools Query`, `Power query`).
  * guest execution: writes `job.cmd` into `C:\blook\runs\<id>`, runs it with
    `vmrun runProgramInGuest`, pulls `stdout.txt`/`stderr.txt`/`exit.txt` back
    through `copyFileFromGuestToHost`. No shared folders, no guest network.
  * `--elevated` runs the job through a `SYSTEM` scheduled task, so it works
    even when UAC is enabled.

### `state/`

* `png.cc` — WIC decode to 24bpp BGR, grid sampling, colour ratios, a coarse
  fingerprint for hang detection. Deliberately independent from any image
  viewer (the lab host/agent cannot view images).
* `classify.cc` — pure classification of `ScreenStats` + OCR text, plus
  stop-code extraction. Unit tested with synthetic statistics.
* `ocr.cc` — tesseract bridge (chi_sim+eng, falling back to eng), optional.
* `vmwlog.cc` — extracts meaningful `vmware.log` and serial-log lines
  (panics, `Msg_Post`, media errors) while filtering VMware's harmless
  "config file missing" noise.
* `types.cc` — the JSON projections of `bvmenv.state.v1`, `bvmenv.exec.v1`,
  `bvmenv.e2e.v1` and the enum parsers used by downstream tools.

### `lab/`

* `create` — linked clone from the `base` snapshot, per-clone identity fixup,
  boot, wait for guest ops, then snapshot `ready` (memory state included) so
  `reset` is a fast revert.
* `reset` — hard stop, revert to `ready` (fallback: `base`, then
  delete + re-clone). Returns a `bvmenv.reset.v1`-shaped report with timing.
* `state` — samples frame + tools + logs, and loops for hang detection.
* `e2e` — push bundle, run entry point, sample state, classify outcome, collect
  artifacts, reset. See [`e2e.md`](e2e.md).
* `base_build` — see [`base-image.md`](base-image.md).
* `base_verify` — forks a throwaway VM, runs `guest/blook/verify.ps1` plus
  `nested-virt-check.exe`, asserts the base image contract, deletes the fork
  (the base image itself is never dirtied).
* `doctor` — machine readable environment check for downstream self-service.

## Why subprocesses instead of VIX COM

* `vmrun`/`vmcli` are stable, scriptable, and their failure modes are visible
  in the `vmware.log`. A crash of the CLI cannot leave the VM in a wedged
  state, and every bug is reproducible by copy/pasting the logged command line
  (`--verbose` writes them to `<vm_root>\bvmenv.log`).
* `vmcli MKS captureScreenshot` is the only reliable way to see a frame when
  the guest OS cannot answer — VIX's `captureScreen` is a guest operation and
  requires VMware Tools, which is exactly what is missing on a BSOD.

## Timeouts, orphan killing and host safety

* Every external process (`vmrun`, `vmcli`, `vmware-vdiskmanager`, `diskpart`,
  `dism`, `bcdboot`, `powershell`, `tesseract`) is spawned by `run_process`,
  which:
  * starts the child **suspended**, assigns it to a Job Object with
    `KILL_ON_JOB_CLOSE`, then resumes it, so a timeout kills the whole tree
    (including grandchildren such as VMware helper processes);
  * reads stdout/stderr through `PeekNamedPipe` with a stop flag. A blocking
    `ReadFile` used to hang forever when a grandchild inherited the pipe handle
    — this was the cause of 45+ minute hangs in an early build;
  * applies a default wall-clock timeout (`BVMENV_CMD_TIMEOUT_MS`, 300 s) to any
    command that does not pass its own;
  * logs command line + duration + exit code when `--verbose` is on
    (`<vm_root>\bvmenv.log`).
* `wait-ready` never relies on a blocking guest call: it fails fast for an
  unknown VM name or a powered-off VM, polls the VMware Tools state every second
  and only then probes guest operations with a 20 s bound.
* **No host execution.** Guest operations are the only way commands run; there
  is no host-mode fallback. Kernel code under test only ever runs inside the
  guest.
* **Host disk safety.** bvmenv may only create/attach/detach its own images
  (`blook-base.vhd`, `answer.vhd`) inside `BVMENV_VM_ROOT`, by explicit path
  (`select vdisk file="..."` + `detach vdisk noerr`). It never enumerates host
  disks and never uses `select disk N`. Detaching anything else throws before a
  single disk command runs; `tests/unit/test_safety.cc` pins that allowlist,
  including the real incident (`C:\DevDrive.vhdx` must be rejected).
  `bvmenv doctor` reports attached file-backed virtual disks read-only.

## Concurrency

* `FileLock` serialises state-changing operations (`create`, `reset`, `delete`,
  `base build`) across processes.
* Every guest execution uses its own `C:\blook\runs\<id>` directory, so
  parallel `exec` calls against different VMs cannot collide.
* VM names are validated (`[A-Za-z0-9_.-]`, no path separators) to keep the VM
  store contained in `<vm_root>\vms`.
