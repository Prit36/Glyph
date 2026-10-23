# Box presentation investigation — 2026-10-23

The v1.2.11 timer made app-side GDI submissions regular, but the user still reported uneven motion on the 1920 × 1080, 165 Hz display. The earlier timings measured completed GDI batches, not independently presented Glyph frames. A fast paint alone did not establish smooth presentation.

Code inspection also found an input-order hazard: a scheduled frame samples the current cursor, but an older queued `WM_MOUSEMOVE` can subsequently replace that sampled box with stale coordinates. During captured box drags, mouse-message handling now samples the current cursor as well. A regression test supplies a stale coordinate while keeping the physical pointer untouched and verifies the resulting box.

## Implementation

The hardware path uses D3D11 and a two-buffer DXGI `FLIP_DISCARD` swap chain, `SetMaximumFrameLatency(1)`, and the frame-latency waitable object. The message loop waits for that object only while box or paint damage is pending. It samples the pointer after the wait. There is no guessed pre-vblank timer on this path, and stationary overlays do not keep presenting or polling.

The existing renderer still computes the same changed regions and rasterizes into its bounded 512 × 512 CPU scratch tile. Each completed tile is uploaded to a retained GPU scene. A GPU copy fills the current back buffer from that scene before `Present(1, 0)`, so every presented frame is complete. This deliberately avoids relying on discarded back-buffer contents or mixing GDI HWND writes with flip presentation. Exposure, text highlights, and toolbar changes use the same presentation path. The queued Win32 paint region is validated and retained while waiting, so it does not repeatedly wake the message loop.

If movement returns to its displayed starting position before a frame wake, an unchanged scene is presented once to complete that acquired opportunity. The next changed frame must not be stranded by consuming an auto-reset readiness signal without presenting. A liveness regression covers this case.

Hardware creation and presentation failures release the swap chain and select the existing GDI path. Capture closure releases the device, textures, wait handle, and retained damage. The scene plus two requested presentation buffers cost about 23.7 MiB at 1080p or 94.9 MiB at 4K, excluding driver allocations. CPU screenshot/scratch memory is unchanged. This is a deliberate memory tradeoff for explicit frame presentation.

## Validation

`tests/presented-pacing.ps1` checks GPU readback against a complete reference GDI render, masking only unused alpha bytes. It exercises changing and reversed boxes, clipped regions, incremental updates spanning multiple tiles, cancellation, no-damage wake liveness, idle waiting, same-HWND recreation, failed initialization cleanup, and a 4K/200% DPI toolbar. Default mode uses hidden windows and generated pixels. It never captures the desktop, runs OCR, moves the pointer, or changes the clipboard.

`tests/presented-pacing.ps1 -Visible` additionally displays a generated 1080p test scene for five seconds under a nominal 1 kHz message producer. GPU readback is kept outside its timing interval. On this machine (RTX 5060, 165 Hz), one run processed **3,289 input messages and 823 frames in five seconds**, with frame-wake intervals of **6.064 ms median / 6.348 ms p99**. An earlier continuously pending run produced 825 frames with 6.061 ms median / 6.193 ms p99. These are application frame opportunities, not physical input-to-photon latency. Within the measured interval, DXGI's sampled presentation statistics showed no extra-refresh observations relative to observed present-ID advances; two IDs were not separately observed in the sampled statistics. This must not be interpreted as proof of zero dropped frames. The CSV is saved before pixel readback and the subsequent functional checks, which intentionally stall or add frames outside the measurement.

The existing UX suite passes, including input ordering and the prior pixel/selection regressions. The GDI fallback scheduler benchmark also passes. The user subsequently tested the preview and confirmed that box drawing felt good on this setup. Attempting live PresentMon recording from the current session returned access denied, so no new PresentMon/ETW display claim is made. Mixed-refresh multi-monitor behavior and hardware device removal still require live validation. The synthetic scene is a presentation-throughput check, not a measurement of mouse sampling or hardware input latency.

## Try and trace

Build with `./build.ps1 -SkipPackage -OutputDirectory dist/flip-preview`, quit an existing Glyph instance, and run `dist/flip-preview/Glyph.exe`. This is a preview, with no release/version bump.

For an optional GPU/display recording, run `./tests/trace-drag-preview.ps1` from an Administrator PowerShell terminal. Its default executable is now the flip preview; `-PreviewPath dist/frame-pacing-preview/Glyph.exe` selects an older comparison build. The existing helper checks that no WPR recording is already active and records the executable hash/PID. Draw boxes and cancel with Esc during the gesture to avoid OCR; close capture after the trace to save diagnostics.

The bounded `--trace-drag` CSV retains the previous columns and appends `present_id`, `displayed_present_id`, `display_refresh_count`, and `display_sync_qpc`. A nonzero present ID identifies successful DXGI submissions. Display statistics describe an earlier completed frame and can repeat or skip observations; zeros indicate unavailable statistics or the GDI path. `submit_qpc` on DXGI records completion of the Present call, not completion of physical scanout. No screenshot pixels or recognized text are logged.

References: [DXGI flip model and GDI HWND restrictions](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model), [frame-latency waitable object](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject), [swap-chain flags](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag).
