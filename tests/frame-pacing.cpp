// Reuse the offscreen fixtures and invalidation observer. This benchmark never
// displays a window or injects input into the user's apps. It measures submission
// scheduling under a 1 kHz message stream, not physical display presentation.
#define main glyphUxSmokeEntry
#include "ux_smoke.cpp"
#undef main

int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        App app; fixture(app, false, 1.0, 1920, 1080); app.hud = {};
        app.window = CreateWindowEx(0, L"STATIC", L"Glyph frame pacing benchmark", WS_POPUP, 0, 0, 1920, 1080,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        check(app.window != nullptr, "Could not create benchmark window");
        app.active = app.dragging = app.boxMode = true; app.trackingMouse = true;
        app.dragOrigin = {20, 20}; app.selectionBox = {20, 20, 40, 40};
        Surface output(1920, 1080); app.render(output.dc, {0, 0, 1920, 1080});
        Region damage; observedWindow = app.window; observedDamage = damage.get();
        check(app.dragClock.start({20, 20}), "Could not create drag clock");
        HANDLE inputTimer = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        check(inputTimer != nullptr, "Could not create input timer");
        LARGE_INTEGER due{}; due.QuadPart = -10000;
        check(SetWaitableTimer(inputTimer, &due, 1, nullptr, nullptr, FALSE) != FALSE, "Could not schedule synthetic input");
        constexpr UINT inputMessage = WM_APP + 105;
        std::jthread input([&](std::stop_token stop) {
            size_t i = 0;
            while (!stop.stop_requested()) {
                if (WaitForSingleObject(inputTimer, 10) == WAIT_OBJECT_0) PostMessage(app.window, inputMessage, ++i, 0);
            }
        });
        LARGE_INTEGER qpcFrequency{}; QueryPerformanceFrequency(&qpcFrequency);
        double frequency = static_cast<double>(qpcFrequency.QuadPart);
        auto start = DragFrameClock::now();
        size_t inputs = 0;
        std::vector<double> late, renderTime; late.reserve(512); renderTime.reserve(512);
        while ((DragFrameClock::now() - start) / frequency < 2.0) {
            HANDLE timer = app.dragClock.pendingTimer();
            DWORD ready = MsgWaitForMultipleObjectsEx(timer ? 1 : 0, timer ? &timer : nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (timer && ready == WAIT_OBJECT_0) {
                int64_t begin = DragFrameClock::now(), target = app.dragClock.scheduledQpc();
                app.presentDragFrame(); renderRegion(app, output, observedDamage); GdiFlush();
                late.push_back((begin - target) / frequency * 1000);
                renderTime.push_back((DragFrameClock::now() - begin) / frequency * 1000);
                SetRectRgn(observedDamage, 0, 0, 0, 0);
                continue;
            }
            MSG message{};
            if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == inputMessage) {
                    ++inputs;
                    app.mouseMove({400 + static_cast<LONG>(message.wParam % 1200), 300 + static_cast<LONG>(message.wParam % 600)});
                } else { TranslateMessage(&message); DispatchMessage(&message); }
            }
        }
        input.request_stop(); input.join(); CancelWaitableTimer(inputTimer); CloseHandle(inputTimer);
        app.dragClock.stop(); observedWindow = nullptr; observedDamage = nullptr;
        app.active = false; DestroyWindow(app.window); app.window = nullptr;
        check(!late.empty(), "Frame clock did not produce frames");
        auto report = [](const char* name, std::vector<double> values) {
            std::sort(values.begin(), values.end());
            std::cout << name << "_median_ms=" << values[values.size()/2]
                << " p99_ms=" << values[(values.size()-1)*99/100] << " max_ms=" << values.back() << '\n';
        };
        std::cout << "input_messages=" << inputs << " scheduled_frames=" << late.size() << " duration_s=2\n";
        report("wake_lateness", late); report("offscreen_frame_work", renderTime);
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
