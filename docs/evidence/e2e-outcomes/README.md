# e2e failure-classification validation

| Case | Expected | Result | Artifact |
| --- | --- | --- | --- |
| command ignores its `--timeout` budget | `outcome: timeout` | **PASS** — `timed_out: true`, `outcome: "timeout"` | `runs/*/result.json` |
| real bugcheck (test-signed driver calls KeBugCheckEx) | `outcome: bsod` + auto-reset | **classification PASS**, e2e-level pending | `bsod-frame.png` (OCR: "Your device ran into a problem and needs to restart", bugcheck params `0x00000000810CB10C`), classified `bsod` by `screenshot --classify` (`reason: bugcheck text detected in OCR`, `stop_code: DRIVER`) |
| real hard hang (HIGH_LEVEL spin driver) | `outcome: hang` + auto-reset | not yet re-run on 0.1.15 | — |

What blocked the e2e-level BSOD/hang cases and what fixed it:

1. `state` could block for minutes on a wedged guest, so `e2e` hung in
   `state_after` instead of classifying. Fixed in **0.1.15**: probes run under a
   hard wall clock (20 s) and `state` answers with host-only facts plus
   `reason: "state probes timed out; guest/host unresponsive"`.
2. `screenshot --classify` did not OCR the frame, so a real bugcheck frame was
   reported as `boot`. Fixed in **0.1.15**: it OCRs and reports `bsod` + reason +
   stop code (verified on the captured frame above).
3. bundle staging ran through the user session and timed out at the logon/lock
   UI. Fixed in **0.1.13/0.1.15** via VIX `createDirectoryInGuest` (best effort;
   the file copy reports genuinely missing directories).

The remaining item is a clean `outcome: "bsod"` / `outcome: "hang"` e2e run with
`reset.performed: true`; with 1 and 2 fixed the harness can now finish those
cases, and `scripts/validate-e2e-outcomes.ps1` runs all three.
