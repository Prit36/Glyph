// Offscreen hot-path benchmark: no desktop capture, OCR, clipboard, or user input.
#define wWinMain glyphApplicationEntry
#include "../src/main.cpp"
#undef wWinMain
#include <iostream>

int main() {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        App app;
        app.window = CreateWindowEx(0, L"STATIC", L"Glyph benchmark", WS_POPUP, 0, 0, 3840, 2160,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        if (!app.window) throw std::runtime_error("Could not create hidden benchmark window");
        app.frame = std::make_shared<Frame>();
        app.frame->image = std::make_unique<Bitmap>(3840, 2160);
        auto pixels = static_cast<uint32_t*>(app.frame->image->pixels);
        for (int i = 0; i < 3840 * 2160; ++i) pixels[i] = (i * 2654435761u) & 0xffffff;
        app.allocateCanvas();
        Bitmap output(3840, 2160);
        HDC dc = CreateCompatibleDC(nullptr); auto previous = SelectObject(dc, output.handle);
        app.dragging = app.boxMode = true;
        app.selectionBox = {100, 100, 3000, 1800};
        app.render(dc, {0, 0, 3840, 2160});
        auto micros = [](auto since) { return std::chrono::duration<double, std::micro>(Clock::now() - since).count(); };
        auto fullStart = Clock::now();
        for (int i = 0; i < 20; ++i) app.render(dc, {0, 0, 3840, 2160});
        GdiFlush();
        std::cout << "full_box_paint_ms=" << micros(fullStart) / 20000 << '\n';
        constexpr int steps = 400;
        uint64_t area = 0, rectangles = 0;
        alignas(RGNDATA) std::array<BYTE, 16384> storage;
        auto data = reinterpret_cast<RGNDATA*>(storage.data());
        auto start = Clock::now();
        for (int i = 0; i < steps; ++i) {
            RECT before = app.selectionBox;
            app.selectionBox.right += i % 2 ? -1 : 1;
            app.selectionBox.bottom += i % 2 ? -1 : 1;
            HRGN dirty = app.boxDamage.between(before, app.selectionBox);
            if (!dirty || !GetRegionData(dirty, static_cast<DWORD>(storage.size()), data)) throw std::runtime_error("Damage failed");
            auto parts = reinterpret_cast<const RECT*>(data->Buffer);
            for (DWORD j = 0; j < data->rdh.nCount; ++j) {
                area += static_cast<uint64_t>(parts[j].right - parts[j].left) * (parts[j].bottom - parts[j].top);
                app.render(dc, parts[j]);
            }
            rectangles += data->rdh.nCount;
        }
        GdiFlush();
        std::cout << "box_us_per_move=" << micros(start) / steps << " pixels_per_move=" << area / steps
            << " rectangles_per_move=" << static_cast<double>(rectangles) / steps
            << " scratch_bytes=" << static_cast<size_t>(app.canvas->width) * app.canvas->height * 4 << '\n';
        SelectObject(dc, previous); DeleteDC(dc);

        app.words.reserve(20000); app.wordRows.reserve(400);
        for (int row = 0; row < 400; ++row) {
            for (int col = 0; col < 50; ++col) {
                Word w; w.text = L"abcdefgh"; w.rect = {col * 60, row * 5, col * 60 + 48, row * 5 + 4};
                w.highlightBand = w.rect; w.line = row;
                for (int c = 0; c <= 8; ++c) w.edges.push_back(col * 60 + c * 6);
                app.words.push_back(std::move(w));
            }
            app.wordRows.push_back({static_cast<size_t>(row * 50), static_cast<size_t>((row + 1) * 50), {0, row * 5, 2988, row * 5 + 4}});
        }
        app.selection.resize(app.words.size()); app.baseSelection = app.selection;
        app.highlights.changed();
        app.boxMode = false; app.anchorWord = 0; app.anchorCharacter = 0;
        app.textEnd = {0, 0};
        app.updateTextSelection({1500, 1501}); // Prepare the index and initial long range outside timing.
        start = Clock::now();
        for (int i = 0; i < 10000; ++i) app.updateTextSelection({1500 + i % 2 * 6, 1501});
        std::cout << "text_us_per_move=" << micros(start) / 10000 << " words=" << app.words.size() << '\n';
        DestroyWindow(app.window); app.window = nullptr;
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
