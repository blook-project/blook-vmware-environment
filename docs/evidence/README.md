# Evidence

Every claim in this repository is backed by artifacts produced by **real runs on
this host** — no dry runs, no mocked VMware. The directory layout is:

```
docs/evidence/<YYYYMMDD>-<slug>/
  summary.json                 # bvmenv.evidence.v1: steps, exit codes, seconds
  run.log                      # full console log of the harness run
  00-doctor.json               # bvmenv.doctor.v1
  01-base-verify-fast.json     # host-side base image checks
  02-base-verify-deep.json     # live guest contract (test signing, VT-x, ...)
  03-create.json               # bvmenv.create.v1 (linked clone timing)
  04-wait-ready.json
  05-exec.txt                  # guest stdout (ver / whoami / marker)
  06-exec-timeout.json         # timeout enforcement
  07-push.json, 09-pull.json   # file transfer
  08-guest-provision.json      # provision.json pulled from the guest
  10-state-desktop.json + 10-desktop.png
  11-state-login.json   + 11-login.png
  12-e2e-nested-virt.json      # nested VT-x inside the guest
  13-e2e-bsod.json             # real bugcheck (KeBugCheckEx driver)
  14-reset-after-bsod.json     # recovery timing
  16-e2e-hang.json             # real hard hang (HIGH_LEVEL spin driver)
  17-reset-after-hang.json     # recovery timing
  19-state-final.json + 19-final.png
  e2e/<run-id>/                # bvmenv e2e artifact dirs (result.json, screen.png, ...)
```

Regenerate everything with:

```powershell
xmake f -y; xmake -y
xmake f -y --guest_drivers=y; xmake -y guest-vtx-probe; xmake -y guest-bsod-driver; xmake -y guest-hang-driver
gsudo -d .\build\windows\x64\releasedbg\bvmenv.exe base build --force
.\build\windows\x64\releasedbg\bvmenv.exe base verify --deep
pwsh scripts/demo-e2e.ps1
```

Expectations the harness asserts (see `summary.json`):

| Step | Expectation |
| --- | --- |
| `base verify --deep` | every check `ok`, `provision.json.failures` empty |
| `create` | linked clone, target < 30 s |
| `exec` | `BVMENV-EXEC-OK` on stdout, exit code 0 |
| `exec --timeout 5` (`ping -n 40`) | `timed_out: true`, killed, non-zero exit |
| `state` on desktop | `screen.classification == "desktop"` |
| `state` on logon UI | `screen.classification == "login"` (not `bsod`) |
| `e2e` BSOD driver | `outcome == "bsod"`, `screen.png` shows the bugcheck |
| `e2e` hang driver | `outcome == "hang"`, frame unchanged, tools dead |
| `reset` | `ok: true`, target < 20 s |

Runs are listed below. Each subdirectory is self-describing through its
`summary.json`.

## Runs

_See subdirectories; the newest run is the authoritative one._
