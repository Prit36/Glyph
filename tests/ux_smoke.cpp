// Exercise the actual overlay renderer and interaction state without capturing
// the desktop, displaying windows, registering hotkeys, or changing the clipboard.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
BOOL observeInvalidateRect(HWND window, const RECT* area, BOOL erase);
BOOL observeInvalidateRgn(HWND window, HRGN region, BOOL erase);
// Hidden windows discard native update regions. Observe the real damage calls
// so incremental rendering can still be checked without showing an overlay.
#define InvalidateRect observeInvalidateRect
#define InvalidateRgn observeInvalidateRgn
#define wWinMain glyphApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
#undef InvalidateRect
#undef InvalidateRgn
#include <fstream>
#include <iostream>

HWND observedWindow{};
HRGN observedDamage{};
BOOL observeInvalidateRect(HWND window, const RECT* area, BOOL erase) {
    if (window == observedWindow && observedDamage && area) {
        HRGN part = CreateRectRgnIndirect(area);
        CombineRgn(observedDamage, observedDamage, part, RGN_OR); DeleteObject(part);
    }
    return InvalidateRect(window, area, erase);
}
BOOL observeInvalidateRgn(HWND window, HRGN region, BOOL erase) {
    if (window == observedWindow && observedDamage && region) CombineRgn(observedDamage, observedDamage, region, RGN_OR);
    return InvalidateRgn(window, region, erase);
}

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Surface {
    Bitmap image;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ previous{};
    Surface(int w, int h) : image(w, h) { previous = SelectObject(dc, image.handle); }
    ~Surface() { SelectObject(dc, previous); DeleteDC(dc); }
};
void saveBitmap(Bitmap& image, const std::filesystem::path& path) {
    GdiFlush();
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + image.width * image.height * 4;
    info.biSize = sizeof(info); info.biWidth = image.width; info.biHeight = -image.height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<char*>(&file), sizeof(file));
    output.write(reinterpret_cast<char*>(&info), sizeof(info));
    output.write(static_cast<char*>(image.pixels), static_cast<std::streamsize>(image.width) * image.height * 4);
    check(output.good(), "Could not save renderer fixture");
}
Word fixtureWord(std::wstring text, RECT rect, int line, int monitor = 0) {
    Word w; w.text = std::move(text); w.rect = rect; w.line = line; w.monitor = monitor;
    return w;
}
void fixture(App& app, bool dark, double scale = 1.0, int width = 1280, int height = 800) {
    app.frame = std::make_shared<Frame>();
    app.frame->desktop = {0, 0, width, height};
    app.frame->monitors = {{0, 0, width, height}};
    app.frame->image = std::make_unique<Bitmap>(width, height);
    app.allocateCanvas(); app.uiScale = scale;
    HDC dc = CreateCompatibleDC(nullptr); auto old = SelectObject(dc, app.frame->image->handle);
    App::fill(dc, {0, 0, width, height}, dark ? RGB(24, 28, 35) : RGB(246, 247, 249));
    COLORREF text = dark ? RGB(239, 243, 248) : RGB(24, 37, 52);
    auto r = [&](int x, int y, int w, int h) { return RECT{app.ui(x), app.ui(y), app.ui(x + w), app.ui(y + h)}; };
    app.hudText(dc, L"Project notes", r(100, 70, 500, 48), app.ui(32), text);
    app.hudText(dc, L"Your text, within reach.", r(100, 125, 600, 40), app.ui(22), text);
    app.hudText(dc, L"Capture only what you need.", r(220, 230, 800, 32), app.ui(24), text);
    app.hudText(dc, L"Add another box, refine a word, and copy when ready.", r(220, 275, 820, 32), app.ui(22), text);
    app.hudText(dc, L"The original screen stays readable while you choose text.", r(100, 430, 1000, 32), app.ui(20), text);
    GdiFlush(); SelectObject(dc, old); DeleteDC(dc);
    app.hud = {static_cast<LONG>((width - app.ui(380)) / 2), height - app.ui(62),
        static_cast<LONG>((width + app.ui(380)) / 2), height - app.ui(16)};
    app.layoutButtons(); app.status = L"Drag a box around the text you need.";
}
void samePixels(Surface& actual, Surface& expected, const char* message) {
    GdiFlush();
    auto a = static_cast<const uint32_t*>(actual.image.pixels), b = static_cast<const uint32_t*>(expected.image.pixels);
    for (int i = 0; i < actual.image.width * actual.image.height; ++i) {
        if ((a[i] & 0xffffff) != (b[i] & 0xffffff)) {
            std::cerr << "Pixel mismatch at " << i % actual.image.width << ',' << i / actual.image.width << '\n';
            saveBitmap(actual.image, L"build/ux-drag-actual.bmp"); saveBitmap(expected.image, L"build/ux-drag-expected.bmp");
            check(false, message);
        }
    }
}
void renderRegion(App& app, Surface& output, HRGN region) {
    DWORD bytes = GetRegionData(region, 0, nullptr);
    check(bytes != 0, "Missing damage region");
    std::vector<BYTE> storage(bytes); auto data = reinterpret_cast<RGNDATA*>(storage.data());
    check(GetRegionData(region, bytes, data) != 0, "Could not read damage region");
    auto parts = reinterpret_cast<const RECT*>(data->Buffer);
    for (DWORD i = 0; i < data->rdh.nCount; ++i) app.render(output.dc, parts[i]);
}
void dragRegressions() {
    uint32_t random = 137;
    auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    for (double scale : {1.0, 1.25, 1.5, 1.75, 2.0, 3.0}) {
        App app; fixture(app, false, scale, 320, 240);
        app.dragging = app.boxMode = true;
        Surface actual(320, 240), expected(320, 240);
        app.render(actual.dc, {0, 0, 320, 240});
        std::vector<RECT> boxes{{0, 0, 320, 240}, {0, 0, 0, 0}, {10, 10, 11, 11}, {10, 10, 11, 180},
            {10, 10, 210, 11}, {20, 20, 250, 200}, {20, 20, 251, 201}, {20, 20, 250, 200},
            {5, 5, 16, 16}, {290, 200, 320, 240}, {0, 0, 1, 1}};
        for (int i = 0; i < 70; ++i) {
            POINT a{static_cast<LONG>(next() % 400) - 40, static_cast<LONG>(next() % 320) - 40};
            POINT b{static_cast<LONG>(next() % 400) - 40, static_cast<LONG>(next() % 320) - 40};
            boxes.push_back(dragRegion(a, b, 320, 240));
        }
        for (RECT box : boxes) {
            RECT old = app.selectionBox; app.selectionBox = box;
            HRGN damage = app.boxDamage.between(old, box, std::max(1, app.ui(1)));
            check(damage != nullptr, "Box damage allocation failed");
            renderRegion(app, actual, damage);
            app.render(expected.dc, {0, 0, 320, 240});
            samePixels(actual, expected, "Box drag must match full repaint at every DPI and size");
        }
    }

    App app; fixture(app, false, 1.0, 400, 260); app.hud = {};
    app.window = CreateWindowEx(0, L"STATIC", L"Glyph selection test", WS_POPUP, 0, 0, 400, 260,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    check(app.window != nullptr, "Could not create hidden selection test window");
    constexpr int tops[]{60, 20, 100, 60, 5, 150, 30, 210};
    for (int row = 0; row < 8; ++row) {
        LONG top = tops[row], bottom = top + (row == 6 ? 90 : 15);
        for (int col = 0; col < 8; ++col) {
            LONG left = 10 + col * 45;
            Word word = fixtureWord(L"A\U0001F600b", {left, top, left + 34, bottom}, row, row % 2);
            word.edges = {left, left + 8, left + 8, left + 20, left + 34};
            word.highlightBand = {left, top - 2, left + 34, bottom + 2};
            app.words.push_back(std::move(word));
        }
        app.wordRows.push_back({static_cast<size_t>(row * 8), static_cast<size_t>((row + 1) * 8), {10, top, 359, bottom}});
    }
    app.highlights.changed();
    // Independent exhaustive oracle covers overlapping rows, added monitors,
    // equal-distance ties, collapsed character advances, and surrogate pairs.
    for (int trial = 0; trial < 1500; ++trial) {
        POINT p{static_cast<LONG>(next() % 480) - 40, static_cast<LONG>(next() % 340) - 40};
        for (bool nearest : {false, true}) {
            int best = -1; double distance = 1e30;
            for (int i = 0; i < static_cast<int>(app.words.size()); ++i) {
                RECT r = app.words[i].rect;
                if (!nearest && !inside(r, p)) continue;
                LONG dx = std::max({r.left - p.x, 0L, p.x - r.right}), dy = std::max({r.top - p.y, 0L, p.y - r.bottom});
                double d = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy * 4;
                if (d < distance) { distance = d; best = i; }
            }
            int character = 0;
            if (best >= 0) {
                const auto& word = app.words[best];
                for (int c = 0; c < length(word); ++c) if (p.x >= (word.edges[c] + word.edges[c + 1]) / 2) character = c + 1;
                if (character > 0 && character < length(word) && low(word.text[character]) && high(word.text[character - 1])) ++character;
            }
            check(app.hit(p, nearest) == std::pair{best, character}, "Indexed hit testing differs from exhaustive search");
        }
    }
    app.active = app.dragging = true; app.boxMode = false;
    Region selectionDamage; observedWindow = app.window; observedDamage = selectionDamage.get();
    Surface actual(400, 260), expected(400, 260);
    for (bool add : {false, true}) for (int anchor : {0, 29, 63}) {
        app.selection.assign(app.words.size(), Span{});
        if (add) for (size_t i = 0; i < app.words.size(); i += 3) app.selection[i] = {1, 3};
        app.baseSelection = app.selection;
        app.anchorWord = anchor; app.anchorCharacter = 3; app.textEnd = {anchor, 3};
        app.render(actual.dc, {0, 0, 400, 260}); SetRectRgn(observedDamage, 0, 0, 0, 0);
        for (int move = 0; move < 300; ++move) {
            POINT p{static_cast<LONG>(next() % 480) - 40, static_cast<LONG>(next() % 340) - 40};
            auto end = app.hit(p, true), start = std::pair{anchor, 3};
            if (start > end) std::swap(start, end);
            auto reference = app.baseSelection;
            for (int i = start.first; i <= end.first; ++i) {
                Span range{i == start.first ? start.second : 0, i == end.first ? end.second : length(app.words[i])};
                if (range.begin >= range.end) continue;
                if (reference[i].begin < reference[i].end) range = {std::min(range.begin, reference[i].begin), std::max(range.end, reference[i].end)};
                reference[i] = range;
            }
            app.updateTextSelection(p);
            for (size_t i = 0; i < reference.size(); ++i) check(app.selection[i].begin == reference[i].begin && app.selection[i].end == reference[i].end,
                "Incremental selection differs after shrinking, crossing anchor, or adding");
            if (move % 15 == 0) {
                renderRegion(app, actual, observedDamage); SetRectRgn(observedDamage, 0, 0, 0, 0);
                app.render(expected.dc, {0, 0, 400, 260});
                try { samePixels(actual, expected, "Incremental text highlights must match full repaint"); }
                catch (...) { std::cerr << "add=" << add << " anchor=" << anchor << " move=" << move << '\n'; throw; }
            }
        }
    }
    observedWindow = nullptr; observedDamage = nullptr;
    app.active = false; DestroyWindow(app.window); app.window = nullptr;

    // Reusing toolbar shapes must reflect every visual state and preserve GDI
    // handles across repeated paints. Compare with forced fresh rasterization.
    App toolbar; fixture(toolbar, false, 1.0, 640, 360);
    Surface cached(640, 360), fresh(640, 360);
    toolbar.selection = {{0, 1}};
    toolbar.render(cached.dc, {0, 0, 640, 360});
    DWORD handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int state = 0; state < 32; ++state) {
        toolbar.hovered = state % 4; toolbar.pressed = (state / 4) % 4; toolbar.focused = (state / 8) % 4;
        toolbar.recognizing = state % 2 != 0; toolbar.copied = state % 3 == 0;
        toolbar.render(cached.dc, {0, 0, 640, 360});
        toolbar.hudStyleScale = 0;
        toolbar.render(fresh.dc, {0, 0, 640, 360});
        samePixels(cached, fresh, "Cached toolbar differs from fresh rendering");
    }
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == handles, "Toolbar repaints must not leak GDI handles");
}
void framePacingRegressions() {
    check(DragFrameClock::deadline(1000, 0, 100, 20, 0) == 1080, "Frame deadline must precede refresh");
    check(DragFrameClock::deadline(1080, 0, 100, 20, 0) == 1080, "Input exactly at deadline must not wait another refresh");
    check(DragFrameClock::deadline(1081, 0, 100, 20, 1080) == 1180, "Late input must use next frame slot");
    check(DragFrameClock::deadline(1001, 0, 100, 20, 1080) == 1180, "Frame slots must not be submitted twice");
    for (int64_t rate : {60, 120, 144, 165, 240}) {
        int64_t period = 10000000 / rate, lead = std::min<int64_t>(period / 3, 15000), previous = 0;
        int64_t now = 10000000;
        for (int input = 0; input < 100; ++input) {
            int64_t target = DragFrameClock::deadline(now, 9990000, period, lead, previous);
            check(target >= now && target >= previous + period, "Refresh scheduler cannot schedule in the past or reuse a slot");
            check(target - now <= period, "Continuous input must not accumulate frame latency");
            previous = target;
            now = target + (input % 5 + 1) * 100;
        }
    }
    App app; fixture(app, false, 1.0, 400, 260); app.hud = {};
    app.window = CreateWindowEx(0, L"STATIC", L"Glyph frame scheduling test", WS_POPUP, 0, 0, 400, 260,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    check(app.window != nullptr, "Could not create hidden frame test window");
    app.active = app.dragging = app.boxMode = true; app.trackingMouse = true;
    app.dragOrigin = {30, 30}; app.selectionBox = {30, 30, 40, 40};
    Surface actual(400, 260), expected(400, 260);
    app.render(actual.dc, {0, 0, 400, 260});
    Region damage; observedWindow = app.window; observedDamage = damage.get();
    DWORD handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    check(app.dragClock.start({30, 30}), "Could not start high-resolution frame clock");
    check(!app.dragClock.pendingTimer(), "Idle drag clock must not wake or poll");
    RECT original = app.selectionBox;
    for (int i = 0; i < 500; ++i) app.mouseMove({50 + i % 300, 50 + i % 190});
    check(app.boxPaintPending && app.dragClock.pendingTimer(), "Mouse burst must arm one deferred paint");
    check(EqualRect(&original, &app.pendingBoxFrom), "Coalesced damage must retain the displayed starting box");
    RECT empty{}; check(GetRgnBox(observedDamage, &empty) == NULLREGION, "Mouse burst must not invalidate each intermediate box");
    app.presentDragFrame();
    check(!app.boxPaintPending && !app.dragClock.pendingTimer(), "Scheduled frame must consume pending damage and disarm timer");
    renderRegion(app, actual, observedDamage); SetRectRgn(observedDamage, 0, 0, 0, 0);
    app.render(expected.dc, {0, 0, 400, 260}); samePixels(actual, expected, "Coalesced box must match full render");
    app.previousBox = original;
    app.mouseMove({10, 10}); app.mouseMove({390, 250}); app.finishDrag(false);
    check(!app.dragClock.pendingTimer() && !app.boxPaintPending, "Cancelled drag must cancel queued frame");
    renderRegion(app, actual, observedDamage);
    app.render(expected.dc, {0, 0, 400, 260}); samePixels(actual, expected, "Cancelling pending drag must restore the original pixels");
    check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= handles + 3, "Frame scheduling must not leak GDI resources");
    // Sampling the current cursor for a frame must not be undone by an older
    // WM_MOUSEMOVE coordinate still in the message queue. Do not move the mouse.
    POINT cursor{}; check(GetCursorPos(&cursor) != FALSE, "Could not read cursor for input-order regression");
    SetWindowPos(app.window, nullptr, cursor.x - 100, cursor.y - 100, 400, 260, SWP_NOZORDER | SWP_NOACTIVATE);
    SetCapture(app.window);
    check(GetCapture() == app.window, "Could not capture test window for input-order regression");
    app.dragging = app.boxMode = true; app.dragOrigin = {30, 30};
    app.mouseMove({390, 250});
    POINT current = app.currentDragPointer({390, 250});
    RECT sampled = dragRegion(app.dragOrigin, current, 400, 260);
    check(EqualRect(&sampled, &app.selectionBox), "Queued mouse coordinates must not rewind a sampled box");
    app.finishDrag(false); ReleaseCapture();
    observedWindow = nullptr; observedDamage = nullptr;
    app.active = false; DestroyWindow(app.window); app.window = nullptr;

    DragTrace trace; trace.enable(L"build/ux-drag-timing.csv");
    for (int i = 0; i < 4100; ++i) trace.record(i, i, 0, 1, 1, {}, {i, 0});
    trace.save();
    std::ifstream recorded("build/ux-drag-timing.csv"); std::string line; size_t rows = 0;
    while (std::getline(recorded, line)) ++rows;
    check(rows == 4097, "Timing diagnostics must retain at most 4096 paint samples");
}
int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        RECT region = dragRegion({900, 500}, {-20, -10}, 800, 600);
        check(region.left == 0 && region.top == 0 && region.right == 800 && region.bottom == 500, "Reverse/clamped capture failed");
        check(App::singleLine(L" \r\nHello\t world\r\n\r\nagain \u2003") == L"Hello world again", "Single-line copy must normalize whitespace");
        Settings settings;
        settings.hotkey = L" Ctrl + Alt + T "; check(App::parseHotkey(settings), "Readable shortcut rejected");
        for (auto invalid : {L"Ctrl+T+", L"Ctrl", L"T", L"F25", L"Ctrl++T"}) {
            settings.hotkey = invalid; check(!App::parseHotkey(settings), "Invalid shortcut accepted");
        }
        App app;
        app.words = {fixtureWord(L"Hello", {0, 0, 50, 20}, 0), fixtureWord(L"world", {60, 0, 110, 20}, 0),
            fixtureWord(L"Next", {0, 30, 50, 50}, 1), fixtureWord(L"Monitor", {200, 0, 270, 20}, 0, 1)};
        app.selection = {{1, 5}, {0, 5}, {0, 4}, {0, 7}};
        check(app.selectedText() == L"ello world\r\nNext\r\n\r\nMonitor", "Copy order or refined text changed");
        check(app.canCopy() && app.selectedWords() == 4, "Ready selection should be copyable");
        app.recognizing = true; app.copy(false); check(app.pendingCopy == 1 && !app.canCopy(), "Copy should queue while reading");
        app.copy(true); check(app.pendingCopy == 2, "Copy and close should queue while reading");
        app.copy(false, true); check(app.pendingCopy == 3, "Single-line copy should queue while reading");
        app.recognizing = false; app.pendingCopy = 0;
        app.previousBox = {20, 20, 200, 100}; app.selectionBox = {30, 30, 31, 31};
        app.dragging = app.boxMode = true; app.finishDrag(true);
        check(EqualRect(&app.selectionBox, &app.previousBox) && app.selectedWords() == 4, "Accidental click must preserve previous capture");
        app.selectionBox = {30, 30, 300, 300}; app.dragging = true; app.finishDrag(false);
        check(EqualRect(&app.selectionBox, &app.previousBox), "Cancelled drag must restore previous region");
        app.words.clear(); app.selection.clear();

        fixture(app, false);
        Surface output(1280, 800);
        app.render(output.dc, {0, 0, 1280, 800}); saveBitmap(output.image, L"build/ux-ready.bmp");
        app.selectionBox = {200, 210, 1060, 330};
        app.render(output.dc, {0, 0, 1280, 800}); saveBitmap(output.image, L"build/ux-box.bmp");
        GdiFlush();
        auto pixel = [&](int x, int y) { return static_cast<uint32_t*>(output.image.pixels)[y * output.image.width + x] & 0xffffff; };
        check(pixel(500, 215) == RGB(249, 247, 246), "Capture interior must preserve the original screenshot");
        check(pixel(198, 250) == 0x5cdbff, "Capture boundary must have a solid accent stroke");
        check(pixel(197, 250) == 0x08111b, "Capture boundary must have a dark contrast stroke");
        check(pixel(180, 215) != pixel(500, 215), "Outside region must remain dimmed");
        app.dragging = app.boxMode = true;
        app.render(output.dc, {0, 0, 1280, 800}); saveBitmap(output.image, L"build/ux-drag.bmp");
        check(pixel(450, 750) != 0x1b1b1e, "Toolbar must hide during box drag");
        app.dragging = false;

        // A real, hidden test-owned window confines invalidation and delivers no
        // input to the user's desktop. Exercise incremental paint vs a full paint.
        app.window = CreateWindowEx(0, L"STATIC", L"Glyph renderer test", WS_POPUP, 0, 0, 1280, 800, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        check(app.window != nullptr, "Could not create hidden test window");
        app.active = true; app.session = 42;
        app.recognizing = true;
        Result stale; stale.id = 41; stale.complete = true;
        stale.words = {fixtureWord(L"stale", {20, 20, 100, 40}, 0)};
        app.accept(stale); check(app.words.empty() && app.recognizing, "Stale OCR result must be ignored");
        Result ready; ready.id = 42; ready.complete = true; ready.language = L"en-US";
        ready.words = {fixtureWord(L"Capture", {220, 235, 307, 260}, 0), fixtureWord(L"only", {317, 235, 365, 260}, 0)};
        app.accept(ready); check(app.selectedWords() == 2 && app.canCopy(), "Completed OCR must select its text");
        check(app.controlAt({app.copyButton.left + 2, app.copyButton.top + 2}) == 1, "Copy hit target mismatch");
        app.render(output.dc, {0, 0, 1280, 800}); saveBitmap(output.image, L"build/ux-selected.bmp");
        GdiFlush();
        int longestIconEdge = 0;
        for (int x = app.copyButton.left + 4; x < app.copyButton.left + 30; ++x) {
            int run = 0;
            for (int y = app.copyButton.top + 2; y < app.copyButton.bottom - 2; ++y) {
                run = pixel(x, y) == 0x202329 ? run + 1 : 0;
                longestIconEdge = std::max(longestIconEdge, run);
            }
        }
        check(longestIconEdge >= 6, "Copy icon must retain solid pixel-aligned edges");
        app.copied = true; app.status = L"12 characters copied. Ctrl-drag to add more; Esc to close.";
        app.render(output.dc, {0, 0, 1280, 800}); saveBitmap(output.image, L"build/ux-copied.bmp");
        app.copied = false; app.recognizing = true; app.status = L"Reading this box. You can press Enter to copy as soon as it's ready.";
        app.render(output.dc, {0, 0, 1280, 800}); saveBitmap(output.image, L"build/ux-reading.bmp");
        app.recognizing = false;
        app.words.clear(); app.wordRows.clear(); app.selection.clear(); app.highlights.clear();
        app.render(output.dc, {0, 0, 1280, 800});
        RECT old = app.selectionBox; app.selectionBox = {350, 160, 900, 400};
        HRGN dirty = app.boxDamage.between(old, app.selectionBox);
        check(dirty != nullptr, "Could not compute box damage");
        DWORD bytes = GetRegionData(dirty, 0, nullptr);
        std::vector<BYTE> data(bytes); auto regions = reinterpret_cast<RGNDATA*>(data.data());
        check(GetRegionData(dirty, bytes, regions) != 0, "Missing incremental repaint region");
        auto rectangles = reinterpret_cast<RECT*>(regions->Buffer);
        for (DWORD i = 0; i < regions->rdh.nCount; ++i) app.render(output.dc, rectangles[i]);
        Surface expected(1280, 800); app.render(expected.dc, {0, 0, 1280, 800}); GdiFlush();
        saveBitmap(output.image, L"build/ux-incremental.bmp");
        saveBitmap(expected.image, L"build/ux-expected.bmp");
        auto actualPixels = static_cast<uint32_t*>(output.image.pixels);
        auto expectedPixels = static_cast<uint32_t*>(expected.image.pixels);
        for (int i = 0; i < 1280 * 800; ++i) {
            if ((actualPixels[i] & 0xffffff) != (expectedPixels[i] & 0xffffff)) {
                std::cerr << "First paint difference at " << i % 1280 << "," << i / 1280 << '\n';
                throw std::runtime_error("Incremental box paint leaves stale pixels");
            }
        }
        app.pendingCopy = 2; app.recognizing = true;
        Result error; error.id = 42; error.complete = true; error.error = L"Test recognition error";
        app.accept(error); check(app.pendingCopy == 0 && !app.lastError.empty(), "OCR failure must cancel queued copy");
        app.active = false; DestroyWindow(app.window); app.window = nullptr;

        for (auto scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            App scaled; int width = static_cast<int>(1280 * scale), height = static_cast<int>(800 * scale);
            fixture(scaled, true, scale, width, height);
            scaled.selectionBox = {scaled.ui(200), scaled.ui(210), scaled.ui(1060), scaled.ui(330)};
            scaled.focused = 3;
            Surface screen(width, height); scaled.render(screen.dc, {0, 0, width, height});
            check(scaled.hud.left >= 0 && scaled.hud.right <= width && scaled.closeButton.right < scaled.hud.right, "Scaled toolbar must fit its screen");
            saveBitmap(screen.image, std::filesystem::path(L"build/ux-dark-") += std::to_wstring(static_cast<int>(scale * 100)) + L".bmp");
        }
        // Load the actual dialog resource to verify layout and keyboard access.
        // No settings writes, dialog activation, or desktop input are involved.
        HWND dialog = CreateDialogParam(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDD_SETTINGS), nullptr,
            [](HWND, UINT, WPARAM, LPARAM) -> INT_PTR { return FALSE; }, 0);
        check(dialog != nullptr, "Settings resource must load");
        SetDlgItemText(dialog, IDC_SHORTCUT, L"Ctrl+Alt+T");
        SendDlgItemMessage(dialog, IDC_LANGUAGE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic (Windows languages)"));
        SendDlgItemMessage(dialog, IDC_LANGUAGE, CB_SETCURSEL, 0, 0);
        RECT client{}; GetClientRect(dialog, &client);
        for (int id : {IDC_SHORTCUT, IDC_LANGUAGE, IDC_STARTUP, IDC_SETTINGS_ERROR, IDOK, IDCANCEL}) {
            HWND control = GetDlgItem(dialog, id); RECT bounds{}; GetWindowRect(control, &bounds);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&bounds), 2);
            check(bounds.left >= 0 && bounds.right <= client.right && bounds.top >= 0 && bounds.bottom <= client.bottom, "Settings controls must fit the dialog");
            check((GetWindowLong(control, GWL_STYLE) & WS_TABSTOP) || id == IDC_SETTINGS_ERROR, "Settings controls must support keyboard focus");
        }
        DestroyWindow(dialog);
        dragRegressions();
        framePacingRegressions();
        std::cout << "PASS: capture geometry, shortcuts, text order, queued copy, drag recovery, stale/error OCR, contrast, DPI fixtures, randomized 100-300% box paints, indexed hits, incremental/additive text selection, toolbar handle stability, frame scheduling/coalescing/cancellation, and bounded timing diagnostics.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
