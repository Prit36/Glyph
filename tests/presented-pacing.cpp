// A real swap-chain/window test using generated fixture pixels. Never captures
// the desktop, moves the pointer, injects input, runs OCR, or changes clipboard.
#define main glyphUxSmokeEntry
#include "ux_smoke.cpp"
#undef main

LRESULT CALLBACK previewProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_PAINT && app) { app->paint(); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProc(window, message, wp, lp);
}

void verifyGpuPixels(App& app) {
    auto desc = D3D11_TEXTURE2D_DESC{}; app.presenter.scene->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> readback;
    winrt::check_hresult(app.presenter.device->CreateTexture2D(&desc, nullptr, readback.put()));
    app.presenter.context->CopyResource(readback.get(), app.presenter.scene.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(app.presenter.context->Map(readback.get(), 0, D3D11_MAP_READ, 0, &mapped));
    Surface expected(app.frame->image->width, app.frame->image->height);
    app.render(expected.dc, {0, 0, expected.image.width, expected.image.height}); GdiFlush();
    bool equal = true;
    for (int y = 0; y < expected.image.height && equal; ++y) {
        auto actual = reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch);
        auto wanted = static_cast<const uint32_t*>(expected.image.pixels) + y * expected.image.width;
        for (int x = 0; x < expected.image.width; ++x) if ((actual[x] & 0xffffff) != (wanted[x] & 0xffffff)) { equal = false; break; }
    }
    app.presenter.context->Unmap(readback.get(), 0);
    check(equal, "GPU retained scene must match the complete GDI reference render");
}

int main(int argc, char**) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASS cls{}; cls.lpfnWndProc = previewProc; cls.hInstance = GetModuleHandle(nullptr);
        cls.lpszClassName = L"GlyphPresentedPacingTest";
        check(RegisterClass(&cls) != 0, "Could not register preview class");
        App app; fixture(app, false, 1, 1920, 1080);
        app.window = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, cls.lpszClassName,
            L"Glyph presentation test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr, cls.hInstance, nullptr);
        check(app.window != nullptr, "Could not create preview window");
        SetWindowLongPtr(app.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
        app.active = app.dragging = app.boxMode = true; app.trackingMouse = true;
        app.dragOrigin = {40, 40}; app.selectionBox = {40, 40, 400, 300};
        check(app.presenter.start(app.window, 1920, 1080), "Could not initialize hardware flip presentation");
        app.dragTrace.enable(L"build/presented-pacing.csv");
        // Default is hidden for pixel/lifecycle checks. Explicit --visible mode
        // displays a generated desktop for a short display-cadence measurement.
        bool visible = argc > 1;
        if (visible) ShowWindow(app.window, SW_SHOWNOACTIVATE);
        app.invalidate(); UpdateWindow(app.window);
        if (!visible) {
            // Hidden HWNDs can discard native damage. Queue an initial full scene.
            SetRectRgn(app.presentedDamage.get(), 0, 0, 1920, 1080); app.presentPending = true;
            observedWindow = app.window; observedDamage = app.presentedDamage.get();
        }
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        int64_t start = DragFrameClock::now(), previous = 0;
        std::vector<double> intervals;
        size_t frames = 0;
        size_t inputs = 0;
        constexpr UINT inputMessage = WM_APP + 105;
        winrt::handle inputTimer;
        std::jthread input;
        if (visible) {
            inputTimer.attach(CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
            check(static_cast<bool>(inputTimer), "Could not create input producer timer");
            LARGE_INTEGER due{}; due.QuadPart = -10000;
            check(SetWaitableTimer(inputTimer.get(), &due, 1, nullptr, nullptr, FALSE) != FALSE, "Could not start input producer");
            input = std::jthread([&](std::stop_token stop) {
                size_t sequence = 0;
                while (!stop.stop_requested()) {
                    if (WaitForSingleObject(inputTimer.get(), 10) == WAIT_OBJECT_0)
                        PostMessage(app.window, inputMessage, ++sequence, 0);
                }
            });
        }
        auto duration = visible ? 5.0 : 0.3;
        while ((DragFrameClock::now() - start) / static_cast<double>(frequency.QuadPart) < duration) {
            HANDLE handle = app.pendingFrame();
            DWORD ready = MsgWaitForMultipleObjectsEx(handle ? 1 : 0, handle ? &handle : nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            check(ready != WAIT_FAILED, "Presentation message wait failed");
            if (handle && ready == WAIT_OBJECT_0) {
                int64_t current = DragFrameClock::now();
                if (previous) intervals.push_back((current - previous) * 1000.0 / frequency.QuadPart);
                previous = current;
                app.presentDragFrame(); ++frames;
                check(app.presenter.active(), "Flip presentation failed during frame submission");
                // Readback synchronizes the GPU; keep it out of the live
                // cadence measurement and verify pixels after that interval.
                if (!visible) verifyGpuPixels(app);
                if (!visible) app.mouseMove({static_cast<LONG>((frames * 137) % 2100) - 100, static_cast<LONG>((frames * 379) % 1200) - 60});
                if (!visible) { // replicate WM_PAINT damage even on a hidden HWND
                    app.applyPendingBoxDamage();
                    app.presentPending = true;
                }
                continue;
            }
            MSG message{};
            if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == inputMessage) {
                    ++inputs;
                    LONG x = static_cast<LONG>(message.wParam % 1800), y = static_cast<LONG>(message.wParam % 1000);
                    app.mouseMove({400 + (x < 900 ? x : 1800 - x), 300 + (y < 500 ? y : 1000 - y)});
                } else { TranslateMessage(&message); DispatchMessage(&message); }
            }
        }
        if (input.joinable()) { input.request_stop(); input.join(); CancelWaitableTimer(inputTimer.get()); }
        // Save only the measurement interval. Readback/cancellation/liveness
        // checks below deliberately stall or add frames and are not cadence data.
        app.dragTrace.save();
        app.dragTrace = {}; // finishDrag also saves diagnostics; freeze this recording
        // Flush final geometry, then check cancellation and static HUD updates.
        app.applyPendingBoxDamage(); app.invalidate(); UpdateWindow(app.window);
        if (!visible) { SetRectRgn(app.presentedDamage.get(), 0, 0, 1920, 1080); app.presentPending = true; }
        check(WaitForSingleObject(app.presenter.ready, 1000) == WAIT_OBJECT_0, "Final frame wait timed out");
        app.presentGpuFrame(); verifyGpuPixels(app);
        POINT unchanged = app.latestPointer;
        app.mouseMove({unchanged.x + 5, unchanged.y + 5}); app.mouseMove(unchanged);
        check(WaitForSingleObject(app.pendingFrame(), 1000) == WAIT_OBJECT_0, "Coalesced no-damage frame wait timed out");
        app.presentGpuFrame();
        app.mouseMove({unchanged.x + 6, unchanged.y + 6});
        check(WaitForSingleObject(app.pendingFrame(), 1000) == WAIT_OBJECT_0, "No-damage wake must not strand the next frame");
        app.presentGpuFrame();
        app.finishDrag(false); app.invalidate(); UpdateWindow(app.window);
        if (!visible) { SetRectRgn(app.presentedDamage.get(), 0, 0, 1920, 1080); app.presentPending = true; }
        check(WaitForSingleObject(app.presenter.ready, 1000) == WAIT_OBJECT_0, "Cancellation frame wait timed out");
        app.presentGpuFrame(); verifyGpuPixels(app);
        check(!app.pendingFrame(), "Stationary overlay must not poll a signaled presentation handle");
        observedWindow = nullptr; observedDamage = nullptr;
        // Recreate on the same HWND at 4K/200% DPI and exercise uploads beyond
        // the first 512px tile, offscreen clipping, and a visible static HUD.
        ShowWindow(app.window, SW_HIDE);
        app.releaseCanvas(); fixture(app, true, 2.0, 3840, 2160);
        app.dragging = app.boxMode = true; app.selectionBox = {80, 70, 3300, 1900};
        app.latestPointer = {3300, 1900}; app.dragOrigin = {80, 70};
        check(app.presenter.start(app.window, 3840, 2160), "Could not recreate presentation at 4K");
        observedWindow = app.window; observedDamage = app.presentedDamage.get();
        SetRectRgn(observedDamage, 0, 0, 3840, 2160); app.presentPending = true;
        check(WaitForSingleObject(app.presenter.ready, 1000) == WAIT_OBJECT_0, "4K initial frame wait timed out");
        app.presentGpuFrame(); verifyGpuPixels(app);
        for (POINT p : {POINT{3400, 2000}, POINT{900, 600}, POINT{-100, 2300}, POINT{3700, 1000}}) {
            app.mouseMove(p); app.applyPendingBoxDamage(); app.presentPending = true;
            check(WaitForSingleObject(app.presenter.ready, 1000) == WAIT_OBJECT_0, "4K incremental frame wait timed out");
            app.presentGpuFrame(); verifyGpuPixels(app);
        }
        app.dragging = false; // force the toolbar back into the composed scene
        SetRectRgn(observedDamage, 0, 0, 3840, 2160); app.presentPending = true;
        check(WaitForSingleObject(app.presenter.ready, 1000) == WAIT_OBJECT_0, "4K toolbar frame wait timed out");
        app.presentGpuFrame(); verifyGpuPixels(app);
        observedWindow = nullptr; observedDamage = nullptr;
        app.presenter.stop();
        check(!app.presenter.start(app.window, 0, 0), "Invalid texture dimensions must select the GDI fallback");
        check(!app.presenter.active() && !app.presenter.ready, "Failed initialization must release the swap chain and wait handle");
        app.active = false; app.releaseCanvas(); DestroyWindow(app.window); app.window = nullptr;
        check(!app.presenter.active() && !app.pendingFrame(), "Closing capture must release presentation resources");
        check(frames > 2, "No presented frames were produced");
        std::sort(intervals.begin(), intervals.end());
        std::cout << "PASS: 1080p/4K GPU/reference pixels, incremental uploads, cancellation, idle wait, recreation, failure cleanup, and 200% DPI HUD.\n";
        std::cout << "frames=" << frames << " inputs=" << inputs << " frame_interval_median_ms=" << intervals[intervals.size()/2]
            << " p99_ms=" << intervals[(intervals.size()-1)*99/100] << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
    catch (winrt::hresult_error const& e) { std::cerr << "FAIL: HRESULT " << std::hex << e.code().value << '\n'; return 1; }
}
