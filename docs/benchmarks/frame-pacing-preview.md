# Frame pacing in v1.2.11

The live v1.2.10 trace showed approximately 12.1 ms display-change intervals while Glyph continued drawing, versus approximately 6.06 ms per refresh at 165 Hz. It did not isolate the cause or provide an exact count of discarded Glyph frames. The renderer uses GDI, and the captured PresentMon output does not have a separate Glyph application present stream.

Version 1.2.11 adds two changes aimed at submission pacing:

- Box mouse moves update the latest logical box and arm a single high-resolution one-shot timer. Damage is calculated from the last displayed logical position to the final position once per scheduled frame, rather than submitting every intermediate mouse position.
- The UI message loop waits for both messages and the timer. A ready frame takes precedence over queued mouse messages, samples the current pointer, and calls `UpdateWindow` directly. Each completed paint explicitly calls `GdiFlush` to submit the last tile's batch.

The timer uses the pointer monitor's refresh rate and DWM's vblank phase when the reported monitor rates agree. It aims to submit up to 2 ms before vblank, rechecking DWM phase during pending input every quarter second to avoid fractional-rate drift. On a monitor with a different compositor rate, it uses that monitor's rate with a local phase. This is an approximation of a submission opportunity, not a guarantee about DWM's internal scheduling or when pixels reach the display.

There are no new render threads, desktop-sized buffers, continuous polling, or global timer-resolution changes. The timer exists only during a box gesture and is armed only when the box has changed. Release and cancellation consume pending damage and close it. Timer creation/arming failure falls back to normal repainting. External paints fold in pending box damage to prevent stale borders.

## Automated validation

`./tests/ux-smoke.ps1` includes refresh-slot calculations for 60/120/144/165/240 Hz, coalescing 500 mouse changes into one pending paint, comparing resulting pixels against a full render, cancellation before the pending frame, timer disarming, GDI resource stability, and the bounded timing log. Existing visual and selection regressions also run.

`./tests/frame-pacing.ps1` uses a hidden test-owned window, a nominal 1 ms input producer, the actual waitable-timer/message wait path, and offscreen rendering. One local two-second run on the 165 Hz desktop processed 1,433 input messages and 330 scheduled frames. Timer wake lateness was 0.307 ms median / 0.615 ms p99; offscreen frame work was 0.190 ms median / 0.427 ms p99. This verifies the scheduling path under input traffic; it does not verify physical display cadence or zero dropped frames.

## Live comparison

Build using `./build.ps1 -SkipPackage -OutputDirectory dist/frame-pacing-preview`. Quit any running released Glyph tray instance. From an Administrator PowerShell terminal, run `./tests/trace-drag-preview.ps1`. The helper starts the preview with bounded app timing diagnostics (or reuses that exact preview if diagnostics are already enabled), starts GPU/desktop-composition tracing, and activates capture. Draw boxes repeatedly for 20 seconds, cancelling each drag with Esc to avoid OCR; after recording, close capture with Esc so timings are saved.

The output is in `build/frame-pacing-trace-<UTC timestamp>/`: `drag.etl`, `capture.json`, and, when launching a new preview, `paint-timing.csv`. When reusing an existing preview, `capture.json` records its existing CSV path in `AppTracePath`. The CSV retains at most 4,096 paint records in a fixed-capacity ring (allocated only when `--trace-drag <CSV path>` is supplied). It records absolute performance-counter input-receipt, paint-start, completed-GDI-submission, and scheduled-deadline timestamps, plus message age, damage size, and box/pointer coordinates. It contains no screen pixels or recognized text. It is saved outside active dragging, not on each frame. Input receipt is not a hardware input timestamp, and cursor sampling at frame time can be newer than the last processed mouse message.

## First live recording

The 2026-10-22 preview capture on the same 1920 × 1080, 165 Hz desktop contained 1,102 scheduled app paints within its 21.467-second ETL window. Paint-to-completed-GDI-submission time was 0.159 ms median, 0.522 ms p99, and 0.731 ms maximum. Timer-wake lateness was 0.296 ms median and 0.591 ms p99. No sampled paint exceeded the 6.06 ms refresh budget, and none finished later than the scheduler's 2 ms pre-vblank lead allowance. The ETL lost no events or buffers.

Reconstructed Glyph-bearing compositor display intervals had a median of 6.061 ms and a 95th percentile of 6.138 ms in this run. The earlier run's corresponding 95th percentile was 12.228 ms, but gesture/pause/input density differed; these are descriptive observations, not a controlled percentage improvement. The earlier early-write gap filter found zero candidates here, but its eligibility is affected by the new submission phase, so it must not be described as proof that 16 drops became zero.

Conservative submission-to-next-GDI-history matching produced five ambiguous extra-refresh outliers despite those paints finishing on time. Event correlation through a mutable GDI redirection surface is not an independent application presentation or physical screen measurement. The supported result is that the app is meeting its render/submission timing budget; zero physical display drops remains unproven. These measurements were taken from the pre-release preview of the v1.2.11 changes.

References: [DWM timing information](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo), [UpdateWindow bypasses the message queue](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatewindow), [GdiFlush batching behavior](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-gdiflush), [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex).
