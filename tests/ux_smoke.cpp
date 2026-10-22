// Exercise the actual overlay renderer and interaction state without capturing
// the desktop, displaying windows, registering hotkeys, or changing the clipboard.
#define wWinMain glyphApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
#include <fstream>
#include <iostream>

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
        HRGN dirty = boxDamage(old, app.selectionBox);
        check(dirty != nullptr, "Could not compute box damage");
        DWORD bytes = GetRegionData(dirty, 0, nullptr);
        std::vector<BYTE> data(bytes); auto regions = reinterpret_cast<RGNDATA*>(data.data());
        check(GetRegionData(dirty, bytes, regions) != 0, "Missing incremental repaint region");
        auto rectangles = reinterpret_cast<RECT*>(regions->Buffer);
        for (DWORD i = 0; i < regions->rdh.nCount; ++i) app.render(output.dc, rectangles[i]);
        DeleteObject(dirty);
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
        std::cout << "PASS: capture geometry, shortcuts, text order, queued copy, drag recovery, stale/error OCR, contrast, incremental rendering, and 100/125/150/175/200% DPI fixtures.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
