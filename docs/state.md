# `bvmenv state` contract (`bvmenv.state.v1`)

`bvmenv state <vm> --json` reports what the **host** can see about a VM. It never
requires the guest OS to be healthy: the frame comes from the hypervisor
(`vmcli MKS captureScreenshot`), the tools state from VMware, the rest from
`vmware.log` / the guest serial log. This is what makes it usable during a
bugcheck, a hard hang, or before the guest userland exists.

## Fields

```json
{
  "schema": "bvmenv.state.v1",
  "vm": "vm-drv",
  "vmx": "C:\\blook-vms\\vms\\vm-drv\\vm-drv.vmx",
  "power": "on|off|suspended|unknown",
  "tools": { "state": "running|installed|not_installed|unknown",
             "version": "13.0.0.0", "running": true },
  "ip": "192.168.152.128",
  "uptime_seconds": 412,
  "hang": { "suspected": false, "samples": 3, "unchanged_samples": 0 },
  "screen": {
    "classification": "desktop|login|bsod|boot|black|hang|unknown",
    "confidence": 0.85,
    "reason": "human readable why",
    "stop_code": "DRIVER_IRQL_NOT_LESS_OR_EQUAL",
    "ocr_text": "first 4 KiB of OCR output",
    "screenshot": "C:\\blook-vms\\_cache\\shots\\vm-drv-20261021T031500Z.png"
  },
  "log": { "issues": [ { "level": "error|warning", "source": "vmware.log|serial.log",
                         "message": "..." } ] },
  "guest_queryable": true,
  "sampled_at": "2026-10-21T03:15:00Z"
}
```

`ip` and `uptime_seconds` are best effort and `null` when the guest cannot be
queried; they never gate the other fields.

## Classification

The classifier (`src/state/classify.cc`) is a pure function of frame statistics
and optional OCR text, which is why it is unit tested with synthetic inputs:

| Class | Signal |
| --- | --- |
| `bsod` | bugcheck text in OCR (authoritative), else a dominantly blue frame with light text |
| `black` | ≥85 % near-black, no light content |
| `boot` | ≥85 % near-black with a little light content (firmware/boot logo) |
| `login` | logon UI markers in OCR (`登录`, `sign in`, `其他用户`, …) |
| `desktop` | busy/colourful frame, or shell markers in OCR, or guest ops answered |
| `hang` | assigned by sampling, see below |
| `unknown` | no rule matched (e.g. no frame could be captured) |

`confidence` and `reason` are part of the contract only as diagnostics: do not
build logic on thresholds, use `classification` (and `stop_code` for bugchecks).

### Stop codes

With an OCR engine available (tesseract on `PATH`, or `BVMENV_OCR_COMMAND`)
`stop_code` contains the symbolic name (`DRIVER_IRQL_NOT_LESS_OR_EQUAL`) or the
raw code (`0x000000D1`) read from the frame, or from the guest serial log when
kernel debugging printed it. Without an engine the field stays empty and the
classification degrades to colour heuristics (still `bsod`).

### Hang detection

`state --samples N` (N ≥ 3) samples the frame repeatedly. A VM is reported as
`hang` when

1. the frame fingerprint is unchanged across consecutive samples, **and**
2. the guest stopped answering (VMware Tools not running or guest ops failing),
   **and**
3. the frame is not a bugcheck screen (a BSOD is classified as `bsod`).

`unchanged_samples` counts the identical consecutive frames, `samples` the total
taken. A desktop that simply does not repaint (e.g. an idle shell) is **not**
a hang because the guest still answers.

## Screenshots

`screenshot <vm> <out.png>` captures the console framebuffer with
`vmcli <vmx> MKS captureScreenshot <out.png>`. This is a host-side operation: it
works while the guest is booting, bugchecked or hung, unlike
`vmrun captureScreen` which needs guest credentials (VMware Tools) and therefore
fails exactly when the lab needs it most.

## Exposing text for humans

`ocr_text` is included so a human (or a downstream agent without image support)
can read what was on screen. `docs/evidence/` stores the raw PNGs as well.
