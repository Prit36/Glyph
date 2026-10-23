#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <shellapi.h>
#include <shlobj.h>
#include <appmodel.h>
#include <shellscalingapi.h>
#include <MemoryBuffer.h>
#include <emmintrin.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>
#include "resource.h"

using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using Clock = std::chrono::steady_clock;
constexpr UINT WM_TRAY = WM_APP + 1, WM_OCR = WM_APP + 2, WM_ENGINE = WM_APP + 3, WM_DRAG_FRAME = WM_APP + 4;
constexpr int HOTKEY_ID = 1;
constexpr wchar_t APP_NAME[] = L"Glyph";

struct Bitmap {
    HBITMAP handle{};
    void* pixels{};
    int width{}, height{};
    Bitmap(int w, int h) : width(w), height(h) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        handle = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!handle) throw std::runtime_error("Cannot allocate screen bitmap");
    }
    ~Bitmap() { if (handle) DeleteObject(handle); }
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
};

#include "presentation.h"

struct Frame {
    RECT desktop{};
    std::vector<RECT> monitors;
    std::unique_ptr<Bitmap> image;
};
struct Word {
    std::wstring text;
    RECT rect{};
    int line{}, monitor{};
    std::vector<int> edges; // UTF-16 character boundaries, projected into screen coordinates
    RECT highlightBand{}; // Shared vertical bounds for neighboring words on the same visual row
};
struct Span { int begin{}, end{}; };
struct WordRow { size_t first{}, end{}; RECT bounds{}; };
struct TextMetrics { std::vector<int> advances; int width{}; };
class FontCache;
int length(const Word& w);
// Heterogeneous lookup avoids copying every word just to probe the cache.
struct MetricsKeyLess {
    using is_transparent = void;
    bool operator()(const auto& a, const auto& b) const {
        if (a.first != b.first) return a.first < b.first;
        return std::wstring_view(a.second) < std::wstring_view(b.second);
    }
};
class TextMetricsCache {
    std::map<std::pair<int, std::wstring>, TextMetrics, MetricsKeyLess> entries;
    TextMetrics scratch;
    size_t payload{};
public:
    // Budget used key/advance bytes; allocator slack and map-node overhead are
    // additional, bounded by the entry limit. Discard this cache after result
    // preparation: character edges already retain what the UI needs.
    static constexpr size_t maxPayload = 64 * 1024, maxEntries = 256;
    const TextMetrics& get(int height, const std::wstring& text, HDC dc, FontCache& fonts);
};
class FontCache {
    std::vector<std::pair<std::pair<int, int>, HFONT>> fonts;
public:
    static constexpr size_t maxFonts = 64;
    ~FontCache() { clear(); }
    HFONT get(int height, int weight = FW_NORMAL) {
        for (auto [size, font] : fonts) if (size == std::pair{height, weight}) return font;
        HFONT font = CreateFont(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) throw std::runtime_error("Cannot create selection font metrics");
        // Callers restore their previous font before requesting another one.
        // Keep exact sizes while bounding handles for boxes with many heights.
        if (fonts.size() == maxFonts) { DeleteObject(fonts.front().second); fonts.erase(fonts.begin()); }
        try { fonts.emplace_back(std::pair{height, weight}, font); }
        catch (...) { DeleteObject(font); throw; }
        return font;
    }
    void clear() { for (auto [size, font] : fonts) DeleteObject(font); fonts.clear(); }
};
const TextMetrics& TextMetricsCache::get(int height, const std::wstring& text, HDC dc, FontCache& fonts) {
    auto found = entries.find(std::pair{height, std::wstring_view(text)});
    if (found != entries.end()) return found->second;
    scratch.advances.resize(text.size());
    auto old = SelectObject(dc, fonts.get(height)); SIZE extent{};
    BOOL ok = GetTextExtentExPoint(dc, text.c_str(), static_cast<int>(text.size()), 0, nullptr, scratch.advances.data(), &extent);
    SelectObject(dc, old); scratch.width = ok ? extent.cx : 0;
    size_t bytes = (text.size() + 1) * sizeof(wchar_t) + text.size() * sizeof(int);
    if (entries.size() < maxEntries && bytes <= maxPayload - payload) {
        auto [entry, inserted] = entries.emplace(std::pair{height, text}, scratch);
        if (inserted) payload += bytes;
        return entry->second;
    }
    return scratch;
}

class HighlightRows {
    struct Row { size_t first{}, end{}; RECT bounds{}; LONG maxBottom{}; };
    std::vector<Row> rows;
    bool dirty = true;
public:
    void changed() { dirty = true; }
    void clear() { std::vector<Row>().swap(rows); dirty = true; }
    void prepare(const std::vector<Word>& words, const std::vector<WordRow>& wordRows) {
        if (!dirty) return;
        rows.clear();
        for (const auto& row : wordRows) {
            RECT bounds = words[row.first].highlightBand;
            for (size_t i = row.first + 1; i < row.end; ++i) UnionRect(&bounds, &bounds, &words[i].highlightBand);
            --bounds.left; ++bounds.right;
            rows.push_back({row.first, row.end, bounds, 0});
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.bounds.top < b.bounds.top; });
        LONG bottom = LONG_MIN;
        for (auto& row : rows) { bottom = std::max(bottom, row.bounds.bottom); row.maxBottom = bottom; }
        dirty = false;
    }
    template<typename Draw>
    void intersecting(RECT damage, const std::vector<Word>& words, const std::vector<Span>& selection, Draw draw) const {
        // Prefix maxima also handle overlapping added boxes and tall lines.
        auto first = std::upper_bound(rows.begin(), rows.end(), damage.top,
            [](LONG top, const auto& row) { return top < row.maxBottom; });
        for (; first != rows.end() && first->bounds.top < damage.bottom; ++first) {
            RECT clipped{};
            if (IntersectRect(&clipped, &first->bounds, &damage)) drawRow(*first, damage, words, selection, draw);
        }
    }
    int hit(POINT p, bool nearest, const std::vector<Word>& words) const {
        int best = -1; double bestDistance = 1e30;
        auto distance = [p](RECT r) {
            LONG dx = std::max({r.left - p.x, 0L, p.x - r.right}), dy = std::max({r.top - p.y, 0L, p.y - r.bottom});
            return static_cast<double>(dx) * dx + static_cast<double>(dy) * dy * 4;
        };
        auto visit = [&](const Row& row) {
            if ((!nearest && !insideBounds(row.bounds, p)) || distance(row.bounds) > bestDistance) return;
            for (size_t i = row.first; i < row.end; ++i) {
                const auto& r = words[i].rect;
                if (!nearest && !insideBounds(r, p)) continue;
                double d = distance(r);
                if (d < bestDistance || (d == bestDistance && static_cast<int>(i) < best)) {
                    bestDistance = d; best = static_cast<int>(i);
                }
            }
        };
        auto split = std::upper_bound(rows.begin(), rows.end(), p.y,
            [](LONG y, const Row& row) { return y < row.bounds.top; });
        // Start at the pointer's row, rather than walking from the first row
        // and repeatedly finding closer words. Prefix bottoms retain correctness
        // for tall lines and overlapping Ctrl-added boxes; ties keep text order.
        for (auto row = split; row != rows.begin();) {
            --row;
            double dy = static_cast<double>(std::max(0L, p.y - row->maxBottom));
            if (dy * dy * 4 > bestDistance) break;
            visit(*row);
        }
        for (auto row = split; row != rows.end(); ++row) {
            double dy = static_cast<double>(row->bounds.top - p.y);
            if (dy * dy * 4 > bestDistance) break;
            visit(*row);
        }
        return best;
    }
private:
    static bool insideBounds(RECT r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
    template<typename Draw>
    static void drawRow(const Row& row, RECT damage, const std::vector<Word>& words, const std::vector<Span>& selection, Draw draw) {
        RECT strip{}; bool haveStrip = false; size_t previousIndex = 0;
        auto append = [&] { RECT clipped{}; if (IntersectRect(&clipped, &strip, &damage)) draw(clipped); };
        for (size_t i = row.first; i < row.end; ++i) {
            const auto& w = words[i]; auto s = selection[i];
            if (s.begin >= s.end) { if (haveStrip) append(); haveStrip = false; continue; }
            RECT next{w.edges[s.begin], w.highlightBand.top, w.edges[s.end], w.highlightBand.bottom};
            if (s.begin == 0) --next.left;
            if (s.end == length(w)) ++next.right;
            bool connect = haveStrip && previousIndex + 1 == i && selection[previousIndex].end == length(words[previousIndex]) && s.begin == 0 &&
                words[previousIndex].line == w.line && words[previousIndex].monitor == w.monitor && strip.top == next.top && strip.bottom == next.bottom &&
                next.left - strip.right <= std::max(12L, (next.bottom - next.top) * 2);
            if (connect) strip.right = std::max(strip.right, next.right);
            else { if (haveStrip) append(); strip = next; haveStrip = true; }
            previousIndex = i;
        }
        if (haveStrip) append();
    }
};
struct Settings { std::wstring hotkey = L"Ctrl+Alt+T", language = L"auto"; UINT modifiers = MOD_CONTROL | MOD_ALT, key = 'T'; bool closeAfterCopy = false; };
struct Job { uint64_t id{}; std::shared_ptr<Frame> frame; RECT region{}; std::wstring language; Clock::time_point started; };
struct Result { uint64_t id{}; std::vector<Word> words; std::wstring error, language; bool complete{}; int elapsed{}; };
struct EngineInfo { std::vector<std::pair<std::wstring, std::wstring>> languages; std::wstring error; };

std::wstring widen(const std::exception& e) {
    int n = MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, nullptr, 0);
    std::wstring value(n > 0 ? n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, value.data(), n);
    if (!value.empty() && !value.back()) value.pop_back();
    return value;
}
bool inside(RECT r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
bool intersects(RECT a, RECT b) { RECT out{}; return IntersectRect(&out, &a, &b) != FALSE; }
int length(const Word& w) { return static_cast<int>(w.text.size()); }
bool high(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool low(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

RECT dragRegion(POINT start, POINT end, int width, int height) {
    start.x = std::clamp(start.x, 0L, static_cast<LONG>(width)); end.x = std::clamp(end.x, 0L, static_cast<LONG>(width));
    start.y = std::clamp(start.y, 0L, static_cast<LONG>(height)); end.y = std::clamp(end.y, 0L, static_cast<LONG>(height));
    return {std::min(start.x, end.x), std::min(start.y, end.y), std::max(start.x, end.x), std::max(start.y, end.y)};
}

std::array<RECT,4> outlineRects(RECT r, int halo = 4) {
    if (IsRectEmpty(&r)) return {};
    InflateRect(&r,halo,halo);
    return {RECT{r.left,r.top,r.right,r.top + halo * 2}, RECT{r.left,r.bottom - halo * 2,r.right,r.bottom},
        RECT{r.left,r.top,r.left + halo * 2,r.bottom}, RECT{r.right - halo * 2,r.top,r.right,r.bottom}};
}

struct Region {
    HRGN handle{};
    ~Region() { if (handle) DeleteObject(handle); }
    Region() = default;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;
    HRGN get() { if (!handle) handle = CreateRectRgn(0, 0, 0, 0); return handle; }
    void clear() { if (handle) DeleteObject(handle); handle = nullptr; }
};
class BoxDamage {
    Region damage, from, to;
public:
    void clear() { damage.clear(); from.clear(); to.clear(); }
    HRGN between(RECT before, RECT after, int unit = 1) {
        if (!damage.get() || !from.get() || !to.get()) return nullptr;
        if (!SetRectRgn(damage.handle, before.left, before.top, before.right, before.bottom) ||
            !SetRectRgn(to.handle, after.left, after.top, after.right, after.bottom) ||
            CombineRgn(damage.handle, damage.handle, to.handle, RGN_XOR) == ERROR) return nullptr;
        // Sweep only edges whose coordinates moved. Unchanged edges need no
        // repaint; their changing endpoints are covered by perpendicular sweeps.
        // Keep each sweep solid: pixel-exact nested XORs fragment into many thin
        // strips, whose extra GDI flushes cost more than this small overdraw.
        auto oldEdges = outlineRects(before, unit * 3), newEdges = outlineRects(after, unit * 3);
        std::array<bool, 4> moved{before.top != after.top, before.bottom != after.bottom,
            before.left != after.left, before.right != after.right};
        for (size_t i = 0; i < moved.size(); ++i) {
            if (!moved[i] && !IsRectEmpty(&before) && !IsRectEmpty(&after)) continue;
            RECT sweep{}; UnionRect(&sweep, &oldEdges[i], &newEdges[i]);
            if (!SetRectRgn(from.handle, sweep.left, sweep.top, sweep.right, sweep.bottom) ||
                CombineRgn(damage.handle, damage.handle, from.handle, RGN_OR) == ERROR) return nullptr;
        }
        return damage.handle;
    }
};

class DragFrameClock {
    HANDLE timer{};
    HMONITOR monitor{};
    int64_t frequency{}, period{}, phase{}, lastTarget{}, lastTimingQuery{};
    int monitorRate{};
    bool running{}, armed{};
public:
    ~DragFrameClock() { stop(); }
    HANDLE pendingTimer() const { return armed ? timer : nullptr; }
    int64_t scheduledQpc() const { return lastTarget; }
    static int64_t now() { LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc); return qpc.QuadPart; }
    // Choose the next submission slot before vblank, never scheduling two
    // submissions into the same slot.
    static int64_t deadline(int64_t current, int64_t origin, int64_t interval, int64_t lead, int64_t previous) {
        int64_t slot = origin - lead;
        if (slot < current) slot += ((current - slot) / interval) * interval;
        if (slot < current) slot += interval;
        return std::max(slot, previous + interval);
    }
    bool start(POINT screenPoint) {
        stop();
        LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); frequency = value.QuadPart;
        timer = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer) timer = CreateWaitableTimerEx(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer) return false;
        running = true; configure(screenPoint); return true;
    }
    bool request(POINT screenPoint) {
        if (!running) return false;
        if (MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST) != monitor) configure(screenPoint);
        if (armed) return true;
        int64_t current = now();
        if (current - lastTimingQuery > frequency / 4) { synchronize(); current = now(); }
        int64_t lead = std::min(period / 2, frequency / 500); // <=2 ms for wake-up, painting, and GDI submission
        int64_t target = deadline(current, phase, period, lead, lastTarget);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(1, (target - current) * 10000000 / frequency);
        if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) { stop(); return false; }
        lastTarget = target; armed = true; return true;
    }
    void consume() { if (armed) CancelWaitableTimer(timer); armed = false; }
    void stop() {
        consume(); if (timer) CloseHandle(timer); timer = nullptr;
        running = false; monitor = nullptr; lastTarget = lastTimingQuery = 0;
    }
private:
    void configure(POINT screenPoint) {
        consume(); monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEX info{}; info.cbSize = sizeof(info);
        DEVMODE mode{}; mode.dmSize = sizeof(mode);
        monitorRate = 60;
        if (GetMonitorInfo(monitor, &info) && EnumDisplaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            monitorRate = static_cast<int>(mode.dmDisplayFrequency);
        period = std::max<int64_t>(1, frequency / monitorRate); phase = now(); lastTarget = 0;
        synchronize();
    }
    void synchronize() {
        lastTimingQuery = now();
        DWM_TIMING_INFO timing{}; timing.cbSize = sizeof(timing);
        // DWM timing is desktop-wide. Use its phase only when its cadence agrees
        // with the pointer's monitor, retaining that monitor's rate otherwise.
        // Recheck occasionally during pending input so fractional refresh rates
        // do not drift away from the compositor over a long drag.
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0 && timing.qpcVBlank > 0 &&
            timing.rateRefresh.uiDenominator && std::lround(static_cast<double>(timing.rateRefresh.uiNumerator) / timing.rateRefresh.uiDenominator) == monitorRate) {
            period = static_cast<int64_t>(timing.qpcRefreshPeriod); phase = static_cast<int64_t>(timing.qpcVBlank);
        }
    }
};

class DragTrace {
    struct Sample { int64_t input{}, begin{}, submit{}, scheduled{}; DWORD messageAge{}; DWORD rectangles{}; uint64_t pixels{}; RECT box{}; POINT pointer{};
        UINT presentId{}; DXGI_FRAME_STATISTICS displayed{}; };
    std::vector<Sample> samples;
    std::filesystem::path path;
    size_t next{};
    int64_t frequency{};
public:
    bool enabled() const { return !path.empty(); }
    void enable(std::filesystem::path output) {
        path = std::filesystem::absolute(std::move(output)); samples.reserve(4096);
        LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); frequency = value.QuadPart;
    }
    void record(int64_t input, int64_t begin, DWORD age, DWORD rectangles, uint64_t pixels, RECT box, POINT pointer, int64_t scheduled = 0,
        UINT presentId = 0, DXGI_FRAME_STATISTICS displayed = {}) {
        if (!enabled()) return;
        Sample sample{input, begin, DragFrameClock::now(), scheduled, age, rectangles, pixels, box, pointer, presentId, displayed};
        if (samples.size() < 4096) samples.push_back(sample);
        else { samples[next] = sample; next = (next + 1) % samples.size(); }
    }
    void save() const noexcept {
        if (!enabled()) return;
        try {
            std::ofstream output(path);
            output << "qpc_frequency,input_qpc,paint_begin_qpc,submit_qpc,scheduled_qpc,message_age_ms,rectangles,pixels,left,top,right,bottom,pointer_x,pointer_y,present_id,displayed_present_id,display_refresh_count,display_sync_qpc\n";
            for (size_t i = 0; i < samples.size(); ++i) {
                const auto& s = samples[(next + i) % samples.size()];
                output << frequency << ',' << s.input << ',' << s.begin << ',' << s.submit << ',' << s.scheduled << ',' << s.messageAge << ',' << s.rectangles << ','
                    << s.pixels << ',' << s.box.left << ',' << s.box.top << ',' << s.box.right << ',' << s.box.bottom << ',' << s.pointer.x << ',' << s.pointer.y << ','
                    << s.presentId << ',' << s.displayed.PresentCount << ',' << s.displayed.PresentRefreshCount << ',' << s.displayed.SyncQPCTime.QuadPart << '\n';
            }
        } catch (...) { /* Diagnostic output cannot interfere with closing capture. */ }
    }
};

std::shared_ptr<Frame> capture() {
    auto frame = std::make_shared<Frame>();
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * h > 150000000)
        throw std::runtime_error("Screen dimensions are unsupported or exceed the capture memory limit");
    frame->desktop = {x, y, x + w, y + h};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM arg) -> BOOL {
        reinterpret_cast<Frame*>(arg)->monitors.push_back(*r); return TRUE;
    }, reinterpret_cast<LPARAM>(frame.get()));
    frame->image = std::make_unique<Bitmap>(w, h);
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    if (!screen || !memory) {
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        throw std::runtime_error("Cannot open screen capture device");
    }
    auto previous = SelectObject(memory, frame->image->handle);
    BOOL ok = BitBlt(memory, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(memory, previous); DeleteDC(memory); ReleaseDC(nullptr, screen);
    if (!ok) throw std::runtime_error("Windows could not capture this desktop");
    return frame;
}

SoftwareBitmap grayscaleTile(const Frame& frame, int x, int y, int w, int h) {
    SoftwareBitmap native(BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);
    {
        auto buffer = native.LockBuffer(BitmapBufferAccessMode::Write);
        auto reference = buffer.CreateReference();
        auto access = reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
        BYTE* dest{}; UINT32 capacity{}; winrt::check_hresult(access->GetBuffer(&dest, &capacity));
        auto plane = buffer.GetPlaneDescription(0);
        if (plane.StartIndex < 0 || plane.Stride < w * 4 || static_cast<uint64_t>(plane.StartIndex) + static_cast<uint64_t>(h - 1) * plane.Stride + static_cast<uint64_t>(w) * 4 > capacity)
            throw std::runtime_error("Windows returned an invalid OCR pixel buffer");
        auto source = static_cast<const uint8_t*>(frame.image->pixels);
        for (int row = 0; row < h; ++row)
            memcpy(dest + plane.StartIndex + static_cast<size_t>(row) * plane.Stride,
                source + (static_cast<size_t>(y + row) * frame.image->width + x) * 4, static_cast<size_t>(w) * 4);
    }
    return SoftwareBitmap::Convert(native, BitmapPixelFormat::Gray8);
}

// Reuse Windows' exact grayscale conversion across passes. The cache belongs to
// one region job and is capped at 16 MiB. Eligible crops convert in <=16 MiB
// BGRA strips; larger crops fall back to the existing bounded OCR tiles.
struct GrayImage {
    RECT region{};
    std::vector<uint8_t> pixels;
    static constexpr size_t maxPixels = 16 * 1024 * 1024;
    template<typename Cancelled>
    static std::unique_ptr<GrayImage> create(const Frame& frame, RECT region, Cancelled cancelled) {
        int width = region.right - region.left, height = region.bottom - region.top;
        if (width <= 0 || height <= 0 || static_cast<size_t>(width) * height > maxPixels || region.left < 0 || region.top < 0 || region.right > frame.image->width || region.bottom > frame.image->height) return {};
        auto image = std::make_unique<GrayImage>(); image->region = region;
        image->pixels.resize(static_cast<size_t>(width) * height);
        int rows = std::max(1, 262144 / width);
        for (int y = 0; y < height; y += rows) {
            if (cancelled()) return {};
            int count = std::min(rows, height - y);
            auto bitmap = grayscaleTile(frame,region.left,region.top + y,width,count);
            auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read); auto reference = buffer.CreateReference();
            BYTE* source{}; UINT32 capacity{};
            winrt::check_hresult(reference.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&source,&capacity));
            auto plane = buffer.GetPlaneDescription(0);
            if (plane.StartIndex < 0 || plane.Stride < width || static_cast<uint64_t>(plane.StartIndex) + static_cast<uint64_t>(count - 1) * plane.Stride + width > capacity)
                throw std::runtime_error("Windows returned an invalid grayscale pixel buffer");
            for (int row = 0; row < count; ++row)
                memcpy(image->pixels.data() + static_cast<size_t>(y + row) * width,
                    source + plane.StartIndex + static_cast<size_t>(row) * plane.Stride,width);
        }
        return image;
    }
};

// Build OCR input separately from the immutable screenshot. Bilinear enlargement
// gives small desktop fonts another chance without changing selection coordinates.
SoftwareBitmap tileBitmap(const Frame& frame, int x, int y, int w, int h, int scale = 1, bool enhance = false, bool binary = false, const GrayImage* cached = nullptr) {
    SoftwareBitmap grayscale{nullptr};
    if (cached && scale == 1 && !enhance) {
        grayscale = SoftwareBitmap(BitmapPixelFormat::Gray8,w,h,BitmapAlphaMode::Ignore);
        auto buffer = grayscale.LockBuffer(BitmapBufferAccessMode::Write); auto reference = buffer.CreateReference();
        BYTE* dest{}; UINT32 capacity{};
        winrt::check_hresult(reference.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&dest,&capacity));
        auto plane = buffer.GetPlaneDescription(0);
        if (plane.StartIndex < 0 || plane.Stride < w || static_cast<uint64_t>(plane.StartIndex) + static_cast<uint64_t>(h - 1) * plane.Stride + w > capacity)
            throw std::runtime_error("Windows returned an invalid grayscale pixel buffer");
        int width = cached->region.right - cached->region.left;
        for (int row = 0; row < h; ++row)
            memcpy(dest + plane.StartIndex + static_cast<size_t>(row) * plane.Stride,
                cached->pixels.data() + static_cast<size_t>(y - cached->region.top + row) * width + x - cached->region.left,w);
    } else if (!cached) grayscale = grayscaleTile(frame,x,y,w,h);
    if (scale == 1 && !enhance) return grayscale;
    BitmapBuffer inputBuffer{nullptr};
    winrt::Windows::Foundation::IMemoryBufferReference inputReference{nullptr};
    BitmapPlaneDescription inputPlane{};
    const BYTE* input{};
    if (cached) {
        input = cached->pixels.data(); inputPlane.Stride = cached->region.right - cached->region.left;
        inputPlane.StartIndex = (y - cached->region.top) * inputPlane.Stride + x - cached->region.left;
    } else {
        inputBuffer = grayscale.LockBuffer(BitmapBufferAccessMode::Read); inputReference = inputBuffer.CreateReference();
        BYTE* pixels{}; UINT32 capacity{};
        winrt::check_hresult(inputReference.as<::Windows::Foundation::IMemoryBufferByteAccess>()->GetBuffer(&pixels,&capacity));
        input = pixels; inputPlane = inputBuffer.GetPlaneDescription(0);
        if (inputPlane.StartIndex < 0 || inputPlane.Stride < w || static_cast<uint64_t>(inputPlane.StartIndex) + static_cast<uint64_t>(h - 1) * inputPlane.Stride + w > capacity)
            throw std::runtime_error("Windows returned an invalid grayscale pixel buffer");
    }
    SoftwareBitmap bitmap(BitmapPixelFormat::Gray8, w * scale, h * scale, BitmapAlphaMode::Ignore);
    auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
    auto reference = buffer.CreateReference();
    auto access = reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
    BYTE* dest{}; UINT32 capacity{}; winrt::check_hresult(access->GetBuffer(&dest, &capacity));
    auto plane = buffer.GetPlaneDescription(0);
    if (plane.StartIndex < 0 || plane.Stride < w * scale || static_cast<uint64_t>(plane.StartIndex) + static_cast<uint64_t>(h * scale - 1) * plane.Stride + static_cast<uint64_t>(w * scale) > capacity)
        throw std::runtime_error("Windows returned an invalid OCR pixel buffer");
    std::array<uint8_t, 256> tone{};
    std::iota(tone.begin(), tone.end(), uint8_t{0});
    if (enhance) {
        std::array<size_t, 256> histogram{};
        for (int row = 0; row < h; ++row) for (int col = 0; col < w; ++col)
            ++histogram[input[inputPlane.StartIndex + static_cast<size_t>(row) * inputPlane.Stride + col]];
        size_t count = static_cast<size_t>(w) * h, tail = count / 1000, sum = 0;
        int lo = 0, hi = 255, median = 0;
        for (; lo < 255 && sum + histogram[lo] <= tail; ++lo) sum += histogram[lo];
        sum = 0; for (; hi > lo && sum + histogram[hi] <= tail; --hi) sum += histogram[hi];
        sum = 0; for (; median < 255 && sum + histogram[median] < count / 2; ++median) sum += histogram[median];
        if (hi - lo >= 8) for (int value = 0; value < 256; ++value) {
            int stretched = std::clamp((value - lo) * 255 / (hi - lo), 0, 255);
            tone[value] = static_cast<uint8_t>(median < (lo + hi) / 2 ? 255 - stretched : stretched);
            if (binary) tone[value] = tone[value] < 160 ? 0 : 255;
        }
    }
    // Separable integer interpolation preserves the bilinear filter while
    // reusing horizontal rows instead of resampling four pixels per output.
    int denominator = scale * 2, divisor = denominator * denominator;
    struct Column { int first, second, weight; };
    std::vector<Column> columns(static_cast<size_t>(w) * scale);
    for (int col = 0; col < w * scale; ++col) {
        int position = std::clamp(col * 2 + 1 - scale, 0, (w - 1) * denominator);
        int first = position / denominator;
        columns[col] = {first, std::min(first + 1, w - 1), position % denominator};
    }
    // At scales 1–4, horizontal sums fit in 11 bits, and rounded vertical
    // sums fit in 14 bits. Keep rows compact and blend eight samples at once.
    std::vector<uint16_t> topRow(columns.size()), bottomRow(columns.size());
    auto horizontal = [&](int row, std::vector<uint16_t>& values) {
        const auto source = input + inputPlane.StartIndex + static_cast<size_t>(row) * inputPlane.Stride;
        for (size_t col = 0; col < columns.size(); ++col) {
            const auto& c = columns[col];
            values[col] = static_cast<uint16_t>(tone[source[c.first]] * (denominator - c.weight) + tone[source[c.second]] * c.weight);
        }
    };
    int cachedTop = -1, cachedBottom = -1;
    for (int row = 0; row < h * scale; ++row) {
        auto output = dest + plane.StartIndex + static_cast<size_t>(row) * plane.Stride;
        int position = std::clamp(row * 2 + 1 - scale, 0, (h - 1) * denominator);
        int y0 = position / denominator, y1 = std::min(y0 + 1, h - 1), fy = position % denominator;
        if (y0 != cachedTop) {
            if (y0 == cachedBottom) { topRow.swap(bottomRow); cachedBottom = -1; }
            else horizontal(y0, topRow);
            cachedTop = y0;
        }
        if (y1 != cachedBottom) { horizontal(y1, bottomRow); cachedBottom = y1; }
        auto write = [&]<int Divisor>() {
            size_t col = 0;
            if constexpr (Divisor == 16 || Divisor == 36 || Divisor == 64) {
                auto topWeight = _mm_set1_epi16(static_cast<short>(denominator - fy));
                auto bottomWeight = _mm_set1_epi16(static_cast<short>(fy));
                auto rounding = _mm_set1_epi16(Divisor / 2);
                for (; col + 8 <= columns.size(); col += 8) {
                    auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(topRow.data() + col));
                    auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(bottomRow.data() + col));
                    auto sum = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a, topWeight), _mm_mullo_epi16(b, bottomWeight)), rounding);
                    __m128i value;
                    if constexpr (Divisor == 36) {
                        // Exact unsigned 16-bit division: ceil(2^21 / 36).
                        value = _mm_srli_epi16(_mm_mulhi_epu16(sum, _mm_set1_epi16(static_cast<short>(58255))), 5);
                    } else value = _mm_srli_epi16(sum, Divisor == 16 ? 4 : 6);
                    _mm_storel_epi64(reinterpret_cast<__m128i*>(output + col), _mm_packus_epi16(value, value));
                }
            }
            for (; col < columns.size(); ++col)
                output[col] = static_cast<uint8_t>((topRow[col] * (denominator - fy) + bottomRow[col] * fy + Divisor / 2) / Divisor);
        };
        if (scale == 2) write.template operator()<16>();
        else if (scale == 3) write.template operator()<36>();
        else if (scale == 4) write.template operator()<64>();
        else for (size_t col = 0; col < columns.size(); ++col)
            output[col] = static_cast<uint8_t>((topRow[col] * (denominator - fy) + bottomRow[col] * fy + divisor / 2) / divisor);
    }
    reference.Close(); buffer.Close(); return bitmap;
}

struct OcrCandidate { Word word; bool clipped{}; int scale{}, margin{}; unsigned votes{}; bool gapOnly{}; };

bool sameOcrPosition(RECT a, RECT b) {
    RECT overlap{};
    if (!IntersectRect(&overlap, &a, &b)) return false;
    LONG minWidth = std::min(a.right - a.left, b.right - b.left);
    LONG minHeight = std::min(a.bottom - a.top, b.bottom - b.top);
    return (overlap.right - overlap.left) * 2 > minWidth && (overlap.bottom - overlap.top) * 2 > minHeight;
}

std::vector<RECT> ocrTiles(int width, int height, int limit, int scale) {
    // The budget applies to the enlarged bitmap too. Balance both axes so the
    // last tile cannot be a narrow sliver with too little recognition context.
    constexpr int pixelBudget = 5 * 1024 * 1024;
    int maximumWidth = std::min({width, limit / scale, scale >= 3 ? 768 : width});
    int maximumHeight = std::min({limit / scale, pixelBudget / (maximumWidth * scale * scale), scale >= 3 ? 384 : height});
    int overlap = std::min(80, std::min(maximumWidth, maximumHeight) / 4);
    auto dimension = [&](int size, int maximum) {
        int count = std::max(1, (size - overlap + maximum - overlap - 1) / (maximum - overlap));
        return std::min(maximum, (size + overlap * (count - 1) + count - 1) / count);
    };
    int tw = dimension(width, maximumWidth), th = dimension(height, maximumHeight);
    std::vector<RECT> tiles;
    for (int y = 0;; y += th - overlap) {
        for (int x = 0;; x += tw - overlap) {
            tiles.push_back({x, y, std::min(x + tw, width), std::min(y + th, height)});
            if (x + tw >= width) break;
        }
        if (y + th >= height) break;
    }
    return tiles;
}

class OcrPositionIndex {
    std::unordered_map<uint64_t,std::vector<size_t>> cells;
    static uint64_t key(LONG x, LONG y) { return static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32 | static_cast<uint32_t>(x); }
public:
    void add(RECT r, size_t index) {
        for (LONG y = r.top / 32; y <= (r.bottom - 1) / 32; ++y)
            for (LONG x = r.left / 128; x <= (r.right - 1) / 128; ++x) cells[key(x,y)].push_back(index);
    }
    template<typename Match>
    bool any(RECT r, Match match) const {
        for (LONG y = r.top / 32; y <= (r.bottom - 1) / 32; ++y)
            for (LONG x = r.left / 128; x <= (r.right - 1) / 128; ++x) {
                auto found = cells.find(key(x,y)); if (found == cells.end()) continue;
                for (size_t index : found->second) if (match(index)) return true;
            }
        return false;
    }
};

std::vector<Word> mergeOcrCandidates(std::vector<OcrCandidate> candidates) {
    // Release the evidence index before sorting and building the accepted index.
    {
        OcrPositionIndex evidence;
        unsigned availableVotes = 0;
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto& candidate = candidates[i]; const auto& r = candidate.word.rect;
            candidate.votes = 1u << candidate.scale;
            availableVotes |= candidate.votes;
            evidence.add(r,i);
        }
        for (auto& candidate : candidates) {
            const auto& r = candidate.word.rect;
            evidence.any(r,[&](size_t index) {
                const auto& other = candidates[index];
                if (!other.gapOnly && !(candidate.votes & (1u << other.scale)) && sameOcrPosition(r, other.word.rect) &&
                    CompareStringOrdinal(candidate.word.text.c_str(), -1, other.word.text.c_str(), -1, TRUE) == CSTR_EQUAL)
                    candidate.votes |= 1u << other.scale;
                return candidate.votes == availableVotes;
            });
        }
    }
    // Prefer complete readings confirmed at different scales. Duplicate tiles
    // at the same scale are one vote, not extra evidence. Preserve native results
    // on ties for normal-size text; stronger enlargement helps tiny glyphs.
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.gapOnly != b.gapOnly) return !a.gapOnly;
        if (a.clipped != b.clipped) return !a.clipped;
        if (std::popcount(a.votes) != std::popcount(b.votes)) return std::popcount(a.votes) > std::popcount(b.votes);
        auto preference = [](const auto& candidate) {
            return candidate.word.rect.bottom - candidate.word.rect.top < 14 ? 4 - candidate.scale : candidate.scale;
        };
        if (preference(a) != preference(b)) return preference(a) < preference(b);
        if (a.scale != b.scale) return a.scale < b.scale;
        return a.margin > b.margin;
    });
    std::vector<Word> words;
    // Index both axes so long text rows do not require quadratic comparisons.
    OcrPositionIndex accepted;
    for (auto& candidate : candidates) {
        const RECT& r = candidate.word.rect;
        bool duplicate = accepted.any(r,[&](size_t index) { return sameOcrPosition(words[index].rect,r); });
        if (duplicate) continue;
        accepted.add(r,words.size());
        words.push_back(std::move(candidate.word));
    }
    return words;
}

OcrCandidate ocrCandidate(std::wstring text, winrt::Windows::Foundation::Rect r, RECT tile, int scale, int monitor, double degrees = 0) {
    // Windows returns boxes in its deskewed image. Rotate all four corners back
    // around the tile center before comparing passes or placing selections.
    double angle = degrees * 0.017453292519943295, cosine = std::cos(angle), sine = std::sin(angle);
    double cx = (tile.right - tile.left) / 2.0, cy = (tile.bottom - tile.top) / 2.0;
    double left = 1e30, top = 1e30, right = -1e30, bottom = -1e30;
    for (double x : {static_cast<double>(r.X) / scale, static_cast<double>(r.X + r.Width) / scale})
        for (double y : {static_cast<double>(r.Y) / scale, static_cast<double>(r.Y + r.Height) / scale}) {
            double px = cx + (x - cx) * cosine - (y - cy) * sine;
            double py = cy + (x - cx) * sine + (y - cy) * cosine;
            left = std::min(left, px); top = std::min(top, py); right = std::max(right, px); bottom = std::max(bottom, py);
        }
    RECT bounds{tile.left + static_cast<LONG>(std::floor(left)), tile.top + static_cast<LONG>(std::floor(top)),
        tile.left + static_cast<LONG>(std::ceil(right)), tile.top + static_cast<LONG>(std::ceil(bottom))};
    IntersectRect(&bounds, &bounds, &tile);
    int margin = static_cast<int>(std::min({bounds.left - tile.left, bounds.top - tile.top, tile.right - bounds.right, tile.bottom - bounds.bottom}));
    return {Word{std::move(text), bounds, 0, monitor, {}}, margin <= 2, scale, margin};
}

OcrEngine createEngine(const std::wstring& language) {
    OcrEngine engine{nullptr};
    if (language == L"auto") {
        engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) {
            auto available = OcrEngine::AvailableRecognizerLanguages();
            if (available.Size()) engine = OcrEngine::TryCreateFromLanguage(available.GetAt(0));
        }
    } else engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(language));
    if (!engine) throw std::runtime_error("No matching Windows OCR language. Install its OCR language feature in Windows Settings, or choose an installed language from the tray menu.");
    return engine;
}

class EnginePool {
    std::wstring language;
    std::mutex mtx;
    std::vector<OcrEngine> pool;
public:
    explicit EnginePool(std::wstring lang = L"auto") : language(std::move(lang)) { pool.reserve(4); }
    void setLanguage(const std::wstring& lang) {
        std::lock_guard lock(mtx);
        if (language != lang) {
            language = lang;
            pool.clear();
        }
    }
    OcrEngine acquire() {
        std::wstring currentLanguage;
        {
            std::lock_guard lock(mtx);
            if (!pool.empty()) {
                auto e = std::move(pool.back());
                pool.pop_back();
                return e;
            }
            currentLanguage = language;
        }
        // Initialization can be slow; independent engines need not serialize.
        // Language changes happen only between fully drained region jobs.
        return createEngine(currentLanguage);
    }
    void release(OcrEngine e) noexcept {
        try {
            std::lock_guard lock(mtx);
            if (pool.size() < 4) pool.push_back(std::move(e));
        } catch (...) {} // Recycling failure must not interrupt task completion.
    }
    void clear() {
        std::lock_guard lock(mtx);
        pool.clear();
    }
};

class ThreadPool {
    std::vector<std::thread> workers;
    std::queue<std::function<void(std::exception_ptr)>> tasks;
    std::mutex queueMutex;
    std::condition_variable cv;
    bool stop = false;
public:
    explicit ThreadPool(size_t threads) {
        workers.reserve(threads);
        try {
            for (size_t i = 0; i < threads; ++i) {
                workers.emplace_back([this]() {
                    std::exception_ptr apartmentError;
                    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
                    catch (...) { apartmentError = std::current_exception(); }
                    while (true) {
                        std::function<void(std::exception_ptr)> task;
                        {
                            std::unique_lock lock(queueMutex);
                            cv.wait(lock, [this]() { return stop || !tasks.empty(); });
                            if (stop && tasks.empty()) break;
                            task = std::move(tasks.front());
                            tasks.pop();
                        }
                        // parallelFor records initialization and task failures on
                        // the submitting thread, after all references are drained.
                        task(apartmentError);
                    }
                    if (!apartmentError) winrt::uninit_apartment();
                });
            }
        } catch (...) { shutdown(); throw; }
    }
private:
    void enqueue(std::function<void(std::exception_ptr)> task) {
        {
            std::unique_lock lock(queueMutex);
            tasks.push(std::move(task));
        }
        cv.notify_one();
    }
public:
    template<typename Function>
    void parallelFor(size_t count, Function function) {
        if (!count) return;
        struct Group {
            std::mutex mutex;
            std::condition_variable done;
            std::atomic<size_t> next{0};
            size_t remaining{};
            std::exception_ptr error;
        } group;
        // Queue only one runner per worker, regardless of desktop/tile count.
        // Each runner takes the next tile dynamically to balance uneven OCR time.
        try {
            for (size_t runner = 0; runner < std::min(count, workers.size()); ++runner) {
                { std::lock_guard lock(group.mutex); ++group.remaining; }
                try {
                    enqueue([&](std::exception_ptr apartmentError) {
                        try {
                            if (apartmentError) std::rethrow_exception(apartmentError);
                            for (;;) {
                                size_t i = group.next.fetch_add(1, std::memory_order_relaxed);
                                if (i >= count) break;
                                if (!function(i)) { group.next.store(count, std::memory_order_relaxed); break; }
                            }
                        } catch (...) {
                            group.next.store(count, std::memory_order_relaxed);
                            std::lock_guard lock(group.mutex);
                            if (!group.error) group.error = std::current_exception();
                        }
                        // Notify while holding the lock: the submitting thread
                        // must not destroy the group until this last access ends.
                        std::lock_guard lock(group.mutex);
                        --group.remaining; group.done.notify_one();
                    });
                } catch (...) {
                    std::lock_guard lock(group.mutex); --group.remaining;
                    throw;
                }
            }
        } catch (...) {
            group.next.store(count, std::memory_order_relaxed);
            std::lock_guard lock(group.mutex);
            if (!group.error) group.error = std::current_exception();
        }
        // Cancellation stops new OCR work, but running calls still own frame,
        // grayscale, result and callback references. Always wait for those calls.
        std::unique_lock lock(group.mutex);
        group.done.wait(lock, [&] { return group.remaining == 0; });
        if (group.error) std::rethrow_exception(group.error);
    }
    ~ThreadPool() { shutdown(); }
private:
    void shutdown() {
        {
            std::unique_lock lock(queueMutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
};

template<typename Cancelled>
std::vector<Word> recognizeRegion(const Frame& frame, RECT region, int monitor, EnginePool& engines, ThreadPool& pool, int limit, Cancelled cancelled) {
    struct TaskInfo {
        RECT tile;
        int scale;
        bool enhance;
        bool binary;
        bool gapOnly;
    };
    std::vector<TaskInfo> tasks;

    for (int scale : {1, 2, 3, 4}) {
        if (cancelled()) return {};
        for (auto tile : ocrTiles(region.right - region.left, region.bottom - region.top, limit, scale)) {
            if (cancelled()) return {};
            OffsetRect(&tile, region.left, region.top);
            auto pixels = static_cast<const uint32_t*>(frame.image->pixels);
            uint32_t first = pixels[static_cast<size_t>(tile.top) * frame.image->width + tile.left] & 0xffffff;
            bool detail = false;
            for (LONG y = tile.top; y < tile.bottom && !detail; ++y) {
                auto row = pixels + static_cast<size_t>(y) * frame.image->width;
                for (LONG x = tile.left; x < tile.right; ++x) if ((row[x] & 0xffffff) != first) { detail = true; break; }
            }
            if (!detail) continue;

            tasks.push_back({tile, scale, scale >= 3, false, false});
        }
    }
    // Threshold recovery uses the very same 4x crops. Reuse the detail test
    // instead of scanning these screenshot pixels a second time.
    size_t primaryTasks = tasks.size();
    for (size_t i = 0; i < primaryTasks; ++i) {
        if (tasks[i].scale != 4) continue;
        auto task = tasks[i]; task.binary = true; task.gapOnly = true;
        tasks.push_back(task);
    }

    if (tasks.empty() || cancelled()) return {};
    auto grayscale = GrayImage::create(frame, region, cancelled);
    if (cancelled()) return {};

    std::vector<std::vector<OcrCandidate>> results(tasks.size());
    pool.parallelFor(tasks.size(), [&](size_t i) {
        if (cancelled()) return false;
        auto engine = engines.acquire();
        // Return engines on allocation, preprocessing and recognition errors.
        struct Recycle {
            EnginePool& pool;
            OcrEngine& engine;
            ~Recycle() { pool.release(std::move(engine)); }
        } recycle{engines, engine};
        const auto& t = tasks[i];
        auto bitmap = tileBitmap(frame, t.tile.left, t.tile.top,
                                 t.tile.right - t.tile.left, t.tile.bottom - t.tile.top,
                                 t.scale, t.enhance, t.binary, grayscale.get());
        if (!cancelled()) {
            auto recognized = engine.RecognizeAsync(bitmap).get();
            auto angle = recognized.TextAngle(); double degrees = angle ? angle.Value() : 0;
            for (auto line : recognized.Lines()) {
                for (auto item : line.Words()) {
                    auto candidate = ocrCandidate(item.Text().c_str(), item.BoundingRect(), t.tile, t.scale, monitor, degrees);
                    candidate.gapOnly = t.gapOnly;
                    const auto& w = candidate.word;
                    if (!w.text.empty() && w.rect.right > w.rect.left && w.rect.bottom > w.rect.top) {
                        results[i].push_back(std::move(candidate));
                    }
                }
            }
        }
        return !cancelled();
    });

    if (cancelled()) return {};
    // No task can access these now. Free them before allocating combined results.
    grayscale.reset();
    std::vector<TaskInfo>().swap(tasks);

    std::vector<OcrCandidate> allCandidates;
    size_t total = 0;
    for (const auto& r : results) total += r.size();
    allCandidates.reserve(total);
    for (auto& r : results) {
        allCandidates.insert(allCandidates.end(), std::make_move_iterator(r.begin()), std::make_move_iterator(r.end()));
        std::vector<OcrCandidate>().swap(r);
    }
    std::vector<std::vector<OcrCandidate>>().swap(results);

    return mergeOcrCandidates(std::move(allCandidates));
}

void appendVisualRows(std::vector<Word>& words, std::vector<Word>& output, int& nextLine) {
    // Keep only indexes while grouping: the source already owns the words.
    // Preserve stable row/word ordering, including ties and tile-seam fragments.
    std::stable_sort(words.begin(), words.end(), [](const Word& a, const Word& b) { return a.rect.top < b.rect.top; });
    LONG tallest = 0;
    for (const auto& word : words) tallest = std::max(tallest, word.rect.bottom - word.rect.top);
    std::vector<std::vector<size_t>> rows;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& w = words[i];
        auto row = rows.rbegin();
        for (; row != rows.rend(); ++row) {
            const auto& anchor = words[row->front()].rect;
            if (w.rect.top - anchor.top > tallest) { row = rows.rend(); break; }
            double centerA = (anchor.top + anchor.bottom) / 2.0, centerB = (w.rect.top + w.rect.bottom) / 2.0;
            if (std::abs(centerA - centerB) <= std::max(3.0, std::min(anchor.bottom - anchor.top, w.rect.bottom - w.rect.top) * .45)) break;
        }
        if (row == rows.rend()) { rows.emplace_back(); rows.back().push_back(i); }
        else row->push_back(i);
    }
    output.reserve(output.size() + words.size());
    for (auto& row : rows) {
        std::stable_sort(row.begin(), row.end(), [&](size_t a, size_t b) { return words[a].rect.left < words[b].rect.left; });
        int line = nextLine++;
        for (size_t i : row) { words[i].line = line; output.push_back(std::move(words[i])); }
    }
}

// Native x64 includes SSE2. Blend four BGRA pixels per iteration directly into
// the existing canvas, avoiding full-screen GDI fills and AlphaBlend surfaces.
// Original screenshot pixels stay immutable for OCR and crisp selected text.
void blendRect(Bitmap& destination, const Bitmap& source, RECT rect, uint32_t color, int alpha, POINT origin = {}) {
    if (!alpha) {
        auto from = static_cast<const uint32_t*>(source.pixels); auto to = static_cast<uint32_t*>(destination.pixels);
        for (LONG y = rect.top; y < rect.bottom; ++y)
            memcpy(to + static_cast<size_t>(y - origin.y) * destination.width + rect.left - origin.x,
                from + static_cast<size_t>(y) * source.width + rect.left, static_cast<size_t>(rect.right - rect.left) * 4);
        return;
    }
    __m128i zero = _mm_setzero_si128(), ones = _mm_set1_epi16(1);
    __m128i weight = _mm_set1_epi16(static_cast<short>(255 - alpha));
    __m128i colors = _mm_unpacklo_epi8(_mm_set1_epi32(static_cast<int>(color)), zero);
    __m128i constant = _mm_add_epi16(_mm_mullo_epi16(colors, _mm_set1_epi16(static_cast<short>(alpha))), _mm_set1_epi16(128));
    auto blend = [&](const __m128i& pixels) {
        __m128i value = _mm_add_epi16(_mm_mullo_epi16(pixels, weight), constant);
        return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(value, _mm_srli_epi16(value, 8)), ones), 8);
    };
    auto from = static_cast<const uint32_t*>(source.pixels); auto to = static_cast<uint32_t*>(destination.pixels);
    for (LONG y = rect.top; y < rect.bottom; ++y) {
        size_t offset = static_cast<size_t>(y) * source.width + rect.left;
        size_t target = static_cast<size_t>(y - origin.y) * destination.width + rect.left - origin.x;
        LONG width = rect.right - rect.left, x = 0;
        for (; x + 4 <= width; x += 4) {
            __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + offset + x));
            __m128i result = _mm_packus_epi16(blend(_mm_unpacklo_epi8(pixels, zero)), blend(_mm_unpackhi_epi8(pixels, zero)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to + target + x), result);
        }
        for (; x < width; ++x) {
            uint32_t pixel = from[offset + x], output = 0;
            for (int channel = 0; channel < 3; ++channel) {
                int shift = channel * 8;
                int value = ((pixel >> shift) & 255) * (255 - alpha) + ((color >> shift) & 255) * alpha + 128;
                output |= static_cast<uint32_t>((value + (value >> 8) + 1) >> 8) << shift;
            }
            to[target + x] = output;
        }
    }
}


class Worker {
    HWND window;
    ThreadPool pool{std::clamp<size_t>(std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() / 2 : 2, 2, 4)};
    EnginePool engines;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Job> pending;
    bool stopping{};
public:
    std::atomic<uint64_t> generation{0};
    explicit Worker(HWND h) : window(h) { thread = std::thread([this] { run(); }); }
    ~Worker() { stop(); }
    void submit(Job job) { { std::lock_guard lock(mutex); pending = std::move(job); } wake.notify_one(); }
    void cancel() { ++generation; std::lock_guard lock(mutex); pending.reset(); }
    void stop() {
        { std::lock_guard lock(mutex); stopping = true; pending.reset(); }
        ++generation; wake.notify_one();
        if (thread.joinable()) thread.join();
    }
private:
    void send(std::unique_ptr<Result> r) { if (PostMessage(window, WM_OCR, 0, reinterpret_cast<LPARAM>(r.get()))) r.release(); }
    void run() {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch (...) {
            auto info = std::make_unique<EngineInfo>(); info->error = L"Could not initialize Windows Runtime.";
            if (PostMessage(window, WM_ENGINE, 0, reinterpret_cast<LPARAM>(info.get()))) info.release(); return;
        }
        auto info = std::make_unique<EngineInfo>();
        try {
            for (auto language : OcrEngine::AvailableRecognizerLanguages())
                info->languages.emplace_back(language.LanguageTag().c_str(), language.DisplayName().c_str());
            auto testEngine = engines.acquire();
            engines.release(std::move(testEngine));
        } catch (winrt::hresult_error const& e) { info->error = e.message().c_str(); }
        catch (const std::exception& e) { info->error = widen(e); }
        if (PostMessage(window, WM_ENGINE, 0, reinterpret_cast<LPARAM>(info.get()))) info.release();
        for (;;) {
            Job job;
            { std::unique_lock lock(mutex); wake.wait(lock, [this] { return stopping || pending.has_value(); });
              if (stopping) break; job = std::move(*pending); pending.reset(); }
            if (generation != job.id) continue;
            auto result = std::make_unique<Result>(); result->id = job.id;
            try {
                engines.setLanguage(job.language);
                auto currentEngine = engines.acquire();
                result->language = currentEngine.RecognizerLanguage().LanguageTag().c_str();
                int limit = static_cast<int>(OcrEngine::MaxImageDimension());
                engines.release(std::move(currentEngine));
                if (limit <= 128) throw std::runtime_error("Windows reported an invalid OCR image limit");
                int nextLine = 0;
                for (size_t index = 0; index < job.frame->monitors.size(); ++index) {
                    int monitorIndex = static_cast<int>(index);
                    RECT monitor = job.frame->monitors[index], region{};
                    OffsetRect(&monitor, -job.frame->desktop.left, -job.frame->desktop.top);
                    // Jobs contain an explicit image-coordinate crop. No OCR input
                    // may extend outside it, including on mixed/negative monitors.
                    RECT bounds{0, 0, job.frame->image->width, job.frame->image->height};
                    if (!IntersectRect(&region, &job.region, &monitor) || !IntersectRect(&region, &region, &bounds)) continue;
                    auto words = recognizeRegion(*job.frame, region, monitorIndex, engines, pool, limit, [&] { return generation != job.id; });
                    if (generation != job.id) break;
                    // Group fragments from tile seams into visual rows, then order words within each row.
                    appendVisualRows(words, result->words, nextLine);
                }
            } catch (winrt::hresult_error const& e) { result->error = e.message().c_str(); }
            catch (const std::exception& e) { result->error = widen(e); }
            if (generation == job.id) {
                result->complete = true;
                result->elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job.started).count());
                send(std::move(result));
            }
            job.frame.reset();
        }
        engines.clear(); winrt::uninit_apartment();
    }
};

class App {
public:
    HWND window{};
    Settings settings;
    std::filesystem::path config;
    std::unique_ptr<Worker> worker;
    std::shared_ptr<Frame> frame;
    std::vector<Word> words;
    std::vector<WordRow> wordRows;
    std::vector<Span> selection, baseSelection;
    FontCache fonts;
    HighlightRows highlights;
    std::vector<std::pair<std::wstring, std::wstring>> languages;
    std::unique_ptr<Bitmap> canvas;
    HDC renderDc{};
    HGDIOBJ previousCanvas{};
    std::unique_ptr<Bitmap> hudCanvas;
    HDC hudDc{};
    HGDIOBJ previousHudCanvas{};
    std::array<int, 6> hudStyle{};
    double hudStyleScale{};
    BoxDamage boxDamage;
    Region paintRegion;
    DragFrameClock dragClock;
    FlipPresenter presenter;
    Region presentedDamage;
    bool presentPending{}, uploadingFrame{};
    DragTrace dragTrace;
    bool boxPaintPending{};
    RECT pendingBoxFrom{};
    POINT latestPointer{};
    int64_t latestInput{};
    DWORD latestMessageTime{};
    HICON icon{};
    bool active{}, recognizing{}, dragging{}, boxMode{}, addRegion{}, previousAddRegion{}, quitting{};
    bool hotkeyRegistered{}, movingHud{}, addNext{}, copied{}, trackingMouse{};
    int hovered{}, pressed{}, focused{}, pendingCopy{};
    uint64_t session{};
    size_t regionWordStart{};
    int anchorWord{-1}, anchorCharacter{}, elapsed{};
    std::pair<int, int> textEnd{-1, 0};
    double uiScale{1.0};
    POINT dragOrigin{}, hudOffset{};
    RECT hud{}, copyButton{}, closeButton{}, selectionBox{}, previousBox{};
    HWND previousWindow{};
    HWND settingsWindow{};
    std::vector<std::wstring> settingsLanguages;
    std::wstring status, detectedLanguage, lastError;
    UINT taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");

    ~App() { releaseCanvas(); if (icon) DestroyIcon(icon); }
    void releaseCanvas() {
        presenter.stop(); presentedDamage.clear(); presentPending = false;
        if (renderDc) { SelectObject(renderDc, previousCanvas); DeleteDC(renderDc); renderDc = nullptr; }
        canvas.reset();
        if (hudDc) { SelectObject(hudDc, previousHudCanvas); DeleteDC(hudDc); hudDc = nullptr; }
        hudCanvas.reset(); hudStyleScale = 0;
    }
    void allocateCanvas() {
        releaseCanvas();
        // A bounded tile buffer replaces the second desktop-sized bitmap.
        // Painting uses global logical coordinates with a translated viewport.
        canvas = std::make_unique<Bitmap>(std::min(frame->image->width,512),std::min(frame->image->height,512));
        renderDc = CreateCompatibleDC(nullptr);
        if (!renderDc) throw std::runtime_error("Cannot create overlay render device");
        previousCanvas = SelectObject(renderDc,canvas->handle);
    }
    int ui(int value) const { return static_cast<int>(std::lround(value * uiScale)); }
    void invalidate() {
        if (!active) return;
        InvalidateRect(window, nullptr, FALSE);
    }
    void invalidate(RECT area) {
        if (!active || !frame) return;
        RECT bounds{0, 0, frame->image->width, frame->image->height}, clipped{};
        if (!IntersectRect(&clipped, &bounds, &area)) return;
        InvalidateRect(window, &clipped, FALSE);
    }
    void invalidateOutline(RECT area) { for (auto edge : outlineRects(area, std::max(4, ui(4)))) invalidate(edge); }
    void invalidateBoxChange(RECT before, RECT after) {
        // Only the area that changed between the clear box and dimmed desktop
        // needs repainting. Retain separate strips instead of a desktop-sized union.
        if (!active) return;
        if (EqualRect(&before, &after)) return;
        HRGN damage = boxDamage.between(before, after, std::max(1, ui(1)));
        if (damage) InvalidateRgn(window, damage, FALSE);
        else invalidate();
    }
    void applyPendingBoxDamage() {
        dragClock.consume();
        if (!std::exchange(boxPaintPending, false)) return;
        invalidateBoxChange(pendingBoxFrom, selectionBox);
    }
    void presentDragFrame() {
        if (presenter.active()) { presentGpuFrame(); return; }
        dragClock.consume();
        if (!active || !dragging || !boxMode || !boxPaintPending || !frame) return;
        // A ready frame takes priority over queued mouse messages. Sample the
        // actual pointer now so a burst of input cannot leave the box behind.
        POINT p{};
        if (GetCapture() == window && GetCursorPos(&p) && ScreenToClient(window, &p)) {
            latestPointer = p;
            selectionBox = dragRegion(dragOrigin, p, frame->image->width, frame->image->height);
        }
        applyPendingBoxDamage();
        // WM_PAINT normally waits behind input. Submit this scheduled frame
        // directly rather than letting a high-polling mouse postpone it.
        UpdateWindow(window);
    }
    // Only include the swap-chain handle in the wait set when pixels changed.
    // A signaled idle swap chain must never turn the message loop into a poll.
    HANDLE pendingFrame() const {
        if (presenter.active()) return (presentPending || boxPaintPending) ? presenter.ready : nullptr;
        return dragClock.pendingTimer();
    }
    POINT currentDragPointer(POINT fallback) const {
        POINT p{};
        if (GetCapture() == window && GetCursorPos(&p) && ScreenToClient(window, &p)) return p;
        return fallback;
    }
    void fallbackToGdi() {
        presenter.stop(); presentedDamage.clear(); presentPending = false;
        if (active && frame && dragging && boxMode)
            dragClock.start({latestPointer.x + frame->desktop.left, latestPointer.y + frame->desktop.top});
        invalidate();
    }
    void presentGpuFrame() {
        if (!active || !frame || !presenter.active()) return;
        if (dragging && boxMode) {
            RECT old = selectionBox;
            latestPointer = currentDragPointer(latestPointer);
            selectionBox = dragRegion(dragOrigin, latestPointer, frame->image->width, frame->image->height);
            if (!EqualRect(&old, &selectionBox) && !boxPaintPending) { pendingBoxFrom = old; boxPaintPending = true; }
        }
        applyPendingBoxDamage();
        UpdateWindow(window); // collect any queued exposure/HUD/box damage
        // WM_PAINT can itself select the fallback if damage allocation fails.
        if (!presenter.active()) { UpdateWindow(window); return; }
        HRGN damage = presentedDamage.get();
        if (!damage) { fallbackToGdi(); UpdateWindow(window); return; }
        // A burst can return to the displayed box before this wake, leaving
        // no damage. Still complete the acquired swap-chain opportunity with
        // the retained scene; otherwise an auto-reset readiness signal could
        // be consumed without Present and strand the next changed frame.
        int64_t begin = DragFrameClock::now();
        constexpr DWORD maxRects = 256;
        alignas(RGNDATA) std::array<BYTE, sizeof(RGNDATAHEADER) + maxRects * sizeof(RECT)> storage;
        auto data = reinterpret_cast<RGNDATA*>(storage.data());
        DWORD bytes = GetRegionData(damage, static_cast<DWORD>(storage.size()), data);
        DWORD rectangles = 0; uint64_t pixels = 0;
        auto draw = [&](RECT r) {
            render(nullptr, r); ++rectangles;
            pixels += static_cast<uint64_t>(r.right - r.left) * (r.bottom - r.top);
        };
        {
            struct UploadScope { bool& flag; ~UploadScope() { flag = false; } } scope{uploadingFrame};
            uploadingFrame = true;
            if (bytes && bytes <= storage.size() && data->rdh.nCount <= maxRects) {
                auto parts = reinterpret_cast<const RECT*>(data->Buffer);
                for (DWORD i = 0; i < data->rdh.nCount; ++i) draw(parts[i]);
            } else {
                RECT bounds{}; GetRgnBox(damage, &bounds); draw(bounds);
            }
        }
        SetRectRgn(damage, 0, 0, 0, 0); presentPending = false;
        if (!presenter.present()) {
            fallbackToGdi(); // device removal/driver failure: repaint using GDI
        } else if (dragging && boxMode && dragTrace.enabled()) {
            dragTrace.record(latestInput, begin, GetTickCount() - latestMessageTime, rectangles, pixels,
                selectionBox, latestPointer, 0, presenter.presentId, presenter.displayed);
        }
    }
    size_t selectedWords() const {
        return std::count_if(selection.begin(), selection.end(), [](Span s) { return s.begin < s.end; });
    }
    bool canCopy() const { return !recognizing && selectedWords() != 0; }
    bool hudVisible() const { return !(dragging && boxMode); }
    RECT monitorWork(POINT p) const {
        POINT screen{p.x + frame->desktop.left, p.y + frame->desktop.top};
        MONITORINFO info{sizeof(info)};
        GetMonitorInfo(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &info);
        OffsetRect(&info.rcWork, -frame->desktop.left, -frame->desktop.top);
        return info.rcWork;
    }
    double monitorScale(POINT p) const {
        POINT screen{p.x + frame->desktop.left, p.y + frame->desktop.top};
        UINT x = 96, y = 96;
        GetDpiForMonitor(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y);
        return x / 96.0;
    }
    void layoutButtons() {
        closeButton = {hud.right - ui(38), hud.top + ui(8), hud.right - ui(8), hud.bottom - ui(8)};
        copyButton = {hud.right - ui(131), hud.top + ui(8), hud.right - ui(49), hud.bottom - ui(8)};
    }
    void placeHud(POINT p, bool top = false) {
        RECT old = hud, work = monitorWork(p);
        uiScale = monitorScale(p);
        LONG width = std::min(static_cast<LONG>(ui(380)), work.right - work.left - ui(24));
        LONG left = work.left + (work.right - work.left - width) / 2;
        LONG height = ui(46), edge = top ? work.top + ui(16) : work.bottom - ui(16) - height;
        hud = {left, edge, left + width, edge + height}; layoutButtons();
        invalidate(old); invalidate(hud);
    }
    void avoidSelection() {
        if (!intersects(hud, selectionBox)) return;
        POINT center{(selectionBox.left + selectionBox.right) / 2, (selectionBox.top + selectionBox.bottom) / 2};
        // Compare docks using the destination monitor's scale and width.
        placeHud(center);
        RECT work = monitorWork(center), overlap{};
        RECT top{hud.left, work.top + ui(16), hud.right, work.top + ui(62)};
        RECT bottom{hud.left, work.bottom - ui(62), hud.right, work.bottom - ui(16)};
        auto area = [&](RECT r) -> int64_t {
            return IntersectRect(&overlap, &r, &selectionBox) ? static_cast<int64_t>(overlap.right - overlap.left) * (overlap.bottom - overlap.top) : 0;
        };
        bool useTop = area(top) < area(bottom);
        placeHud(center, useTop);
    }
    int controlAt(POINT p) const {
        if (!hudVisible()) return 0;
        return inside(copyButton, p) ? 1 : inside(closeButton, p) ? 3 : 0;
    }
    void notify(const std::wstring& text, bool error = false) {
        NOTIFYICONDATA n{sizeof(n)}; n.hWnd = window; n.uID = 1; n.uFlags = NIF_INFO;
        wcscpy_s(n.szInfoTitle, APP_NAME); wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE); n.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
        Shell_NotifyIcon(NIM_MODIFY, &n);
    }
    void tray(bool add) {
        NOTIFYICONDATA n{sizeof(n)}; n.hWnd = window; n.uID = 1; n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        n.uCallbackMessage = WM_TRAY; n.hIcon = icon;
        auto tip = L"Glyph | " + settings.hotkey; wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIcon(add ? NIM_ADD : NIM_MODIFY, &n);
        if (add) { n.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIcon(NIM_SETVERSION, &n); }
    }
    static std::wstring normalize(std::wstring s) {
        s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return iswspace(c); }), s.end());
        std::transform(s.begin(), s.end(), s.begin(), towupper); return s;
    }
    static bool parseHotkey(Settings& s) {
        std::wstring input = normalize(s.hotkey); UINT mods = 0, key = 0;
        if (input.empty() || input.back() == L'+') return false;
        size_t start = 0;
        while (start < input.size()) {
            auto end = input.find(L'+', start); auto part = input.substr(start, end - start);
            if (part == L"CTRL" || part == L"CONTROL") mods |= MOD_CONTROL;
            else if (part == L"ALT") mods |= MOD_ALT;
            else if (part == L"SHIFT") mods |= MOD_SHIFT;
            else if (part == L"WIN") mods |= MOD_WIN;
            else {
                if (key) return false;
                if (part.size() == 1 && ((part[0] >= L'A' && part[0] <= L'Z') || (part[0] >= L'0' && part[0] <= L'9'))) key = part[0];
                else if (part == L"SPACE") key = VK_SPACE;
                else if (part.size() >= 2 && part[0] == L'F') {
                    wchar_t* rest{}; long number = wcstol(part.c_str() + 1, &rest, 10);
                    if (*rest || number < 1 || number > 24) return false; key = VK_F1 + static_cast<UINT>(number) - 1;
                } else return false;
            }
            if (end == std::wstring::npos) break; start = end + 1;
        }
        if (!key || (!mods && !(key >= VK_F1 && key <= VK_F24))) return false;
        s.modifiers = mods; s.key = key; return true;
    }
    bool applySettings(Settings next, std::wstring& error) {
        if (!parseHotkey(next)) { error = L"Use a shortcut such as Ctrl+Alt+T or Shift+F8."; return false; }
        bool changed = (!hotkeyRegistered || next.modifiers != settings.modifiers || next.key != settings.key);
        if (changed) {
            if (hotkeyRegistered) UnregisterHotKey(window, HOTKEY_ID);
            if (!RegisterHotKey(window, HOTKEY_ID, next.modifiers | MOD_NOREPEAT, next.key)) {
                hotkeyRegistered = RegisterHotKey(window, HOTKEY_ID, settings.modifiers | MOD_NOREPEAT, settings.key) != FALSE;
                error = L"That shortcut is in use. Try a different combination."; return false;
            }
            hotkeyRegistered = true;
        }
        settings = std::move(next); tray(false); return true;
    }
    void loadSettings() {
        wchar_t value[256]{};
        Settings next;
        GetPrivateProfileString(L"Glyph", L"hotkey", L"Ctrl+Alt+T", value, 256, config.c_str()); next.hotkey = value;
        GetPrivateProfileString(L"Glyph", L"language", L"auto", value, 256, config.c_str()); next.language = value;
        next.closeAfterCopy = GetPrivateProfileInt(L"Glyph", L"closeAfterCopy", 0, config.c_str()) != 0;
        std::wstring error;
        if (!applySettings(std::move(next), error)) notify(error + L" Open Settings from the tray to change it.", true);
    }
    static INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
        auto app = reinterpret_cast<App*>(GetWindowLongPtr(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            app = reinterpret_cast<App*>(lp); SetWindowLongPtr(dialog, DWLP_USER, lp);
            app->settingsWindow = dialog; app->settingsLanguages = {L"auto"};
            SendDlgItemMessage(dialog, IDC_SHORTCUT, EM_SETLIMITTEXT, 100, 0);
            SetDlgItemText(dialog, IDC_SHORTCUT, app->settings.hotkey.c_str());
            HWND combo = GetDlgItem(dialog, IDC_LANGUAGE);
            SendMessage(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic (Windows languages)"));
            int selected = 0;
            for (size_t i = 0; i < app->languages.size(); ++i) {
                auto label = app->languages[i].second + L" (" + app->languages[i].first + L")";
                SendMessage(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                app->settingsLanguages.push_back(app->languages[i].first);
                if (app->settings.language == app->languages[i].first) selected = static_cast<int>(i + 1);
            }
            // Preserve a configured tag even when its OCR feature is unavailable.
            if (!selected && app->settings.language != L"auto") {
                auto label = app->settings.language + L" (not installed)";
                selected = static_cast<int>(SendMessage(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
                app->settingsLanguages.push_back(app->settings.language);
            }
            SendMessage(combo, CB_SETCURSEL, selected, 0);
            CheckDlgButton(dialog, IDC_STARTUP, app->startupEnabled() ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_CLOSE_AFTER_COPY, app->settings.closeAfterCopy ? BST_CHECKED : BST_UNCHECKED);
            RECT box{}; GetWindowRect(dialog, &box);
            MONITORINFO info{sizeof(info)}; GetMonitorInfo(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &info);
            SetWindowPos(dialog, HWND_TOP, info.rcWork.left + (info.rcWork.right - info.rcWork.left - box.right + box.left) / 2,
                info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - box.bottom + box.top) / 2, 0, 0, SWP_NOSIZE);
            return TRUE;
        }
        if (!app) return FALSE;
        if (message == WM_DESTROY) { app->settingsWindow = nullptr; app->settingsLanguages.clear(); return TRUE; }
        if (message == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(dialog, 0); return TRUE; }
        if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
            wchar_t shortcut[128]{}; GetDlgItemText(dialog, IDC_SHORTCUT, shortcut, 128);
            Settings next = app->settings, old = app->settings; next.hotkey = shortcut;
            next.closeAfterCopy = IsDlgButtonChecked(dialog, IDC_CLOSE_AFTER_COPY) == BST_CHECKED;
            int language = static_cast<int>(SendDlgItemMessage(dialog, IDC_LANGUAGE, CB_GETCURSEL, 0, 0));
            if (language >= 0 && language < static_cast<int>(app->settingsLanguages.size())) next.language = app->settingsLanguages[language];
            std::wstring error;
            if (!app->applySettings(std::move(next), error)) { SetDlgItemText(dialog, IDC_SETTINGS_ERROR, error.c_str()); return TRUE; }
            if (!WritePrivateProfileString(L"Glyph", L"hotkey", app->settings.hotkey.c_str(), app->config.c_str()) ||
                !WritePrivateProfileString(L"Glyph", L"language", app->settings.language.c_str(), app->config.c_str()) ||
                !WritePrivateProfileString(L"Glyph", L"closeAfterCopy", app->settings.closeAfterCopy ? L"1" : L"0", app->config.c_str())) {
                app->applySettings(old, error);
                WritePrivateProfileString(L"Glyph", L"hotkey", old.hotkey.c_str(), app->config.c_str());
                WritePrivateProfileString(L"Glyph", L"language", old.language.c_str(), app->config.c_str());
                WritePrivateProfileString(L"Glyph", L"closeAfterCopy", old.closeAfterCopy ? L"1" : L"0", app->config.c_str());
                SetDlgItemText(dialog, IDC_SETTINGS_ERROR, L"Could not save settings. Check the settings file is writable."); return TRUE;
            }
            if ((IsDlgButtonChecked(dialog, IDC_STARTUP) == BST_CHECKED) != app->startupEnabled() && !app->toggleStartup()) {
                CheckDlgButton(dialog, IDC_STARTUP, app->startupEnabled() ? BST_CHECKED : BST_UNCHECKED);
                SetDlgItemText(dialog, IDC_SETTINGS_ERROR, L"Settings saved. Could not change start at login."); return TRUE;
            }
            EndDialog(dialog, 1); return TRUE;
        }
        return FALSE;
    }
    void showSettings() {
        if (active) exitMode();
        if (DialogBoxParam(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDD_SETTINGS), window, settingsProc, reinterpret_cast<LPARAM>(this)) == -1)
            notify(L"Could not open settings. You can still edit settings.ini in the Glyph application-data folder.", true);
    }
    void initialize() {
        PWSTR path{};
        winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path));
        config = std::filesystem::path(path) / L"Glyph" / L"settings.ini"; CoTaskMemFree(path);
        std::filesystem::create_directories(config.parent_path());
        if (!std::filesystem::exists(config)) {
            WritePrivateProfileString(L"Glyph", L"hotkey", L"Ctrl+Alt+T", config.c_str());
            WritePrivateProfileString(L"Glyph", L"language", L"auto", config.c_str());
        }
        icon = makeIcon(); tray(true); loadSettings();
        worker = std::make_unique<Worker>(window);
        notify(L"Ready in the tray. Press " + settings.hotkey + L" to select screen text. Esc exits OCR mode.");
    }
    static HICON makeIcon() {
        auto result = static_cast<HICON>(LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(1),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (!result) winrt::throw_last_error();
        return result;
    }
    void prepareCaptureFrame() {
        // A hidden HWND may discard invalidation. Explicitly initialize the
        // entire retained scene before its first presentation, including tiles
        // that belonged to the previous capture on this same HWND.
        if (presenter.active()) {
            if (!SetRectRgn(presentedDamage.get(), 0, 0, frame->image->width, frame->image->height)) fallbackToGdi();
            else {
                presentPending = true;
                if (WaitForSingleObject(presenter.ready, 1000) == WAIT_OBJECT_0) presentGpuFrame();
                else fallbackToGdi();
            }
        }
        // Also initialize the GDI surface if hardware startup/presentation failed.
        if (!presenter.active()) { invalidate(); UpdateWindow(window); GdiFlush(); }
    }
    void showCapture() {
        // DWM retains the previous contents of a reused HWND. Keep composing
        // while concealed until the fresh capture has replaced those pixels.
        BOOL conceal = TRUE;
        winrt::check_hresult(DwmSetWindowAttribute(window, DWMWA_CLOAK, &conceal, sizeof(conceal)));
        DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED, &conceal, sizeof(conceal));
        try {
            SetWindowLongPtr(window, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_APPWINDOW);
            if (!SetWindowPos(window, HWND_TOPMOST, frame->desktop.left, frame->desktop.top,
                frame->image->width, frame->image->height, SWP_SHOWWINDOW | SWP_NOACTIVATE)) winrt::throw_last_error();
            presenter.start(window, frame->image->width, frame->image->height);
            prepareCaptureFrame();
            winrt::check_hresult(DwmFlush());
            conceal = FALSE;
            winrt::check_hresult(DwmSetWindowAttribute(window, DWMWA_CLOAK, &conceal, sizeof(conceal)));
        } catch (...) {
            ShowWindow(window, SW_HIDE);
            conceal = FALSE;
            DwmSetWindowAttribute(window, DWMWA_CLOAK, &conceal, sizeof(conceal));
            throw;
        }
    }
    void activate() {
        if (settingsWindow) { SetForegroundWindow(settingsWindow); return; }
        if (active) { exitMode(); return; }
        previousWindow = GetForegroundWindow();
        try {
            frame = capture();
            allocateCanvas();
        }
        catch (const std::exception& e) { frame.reset(); releaseCanvas(); notify(widen(e), true); return; }
        words.clear(); wordRows.clear(); selection.clear(); baseSelection.clear(); detectedLanguage.clear();
        highlights.clear();
        status = L"Drag a box around the text you need.";
        active = true; recognizing = false; dragging = false; boxMode = true; anchorWord = -1; elapsed = 0;
        selectionBox = {}; previousBox = {}; regionWordStart = 0; copied = false; lastError.clear();
        movingHud = false; addNext = false; hovered = pressed = focused = pendingCopy = 0;
        session = ++worker->generation;
        POINT cursor{}; GetCursorPos(&cursor);
        placeHud({cursor.x - frame->desktop.left, cursor.y - frame->desktop.top});
        // Activation only freezes the desktop. Recognition starts on box release.
        try { showCapture(); }
        catch (...) { exitMode(); throw; }
        SetForegroundWindow(window); SetFocus(window);
    }
    void exitMode() {
        if (!active) return;
        dragClock.stop(); boxPaintPending = false;
        bool restoreFocus = GetForegroundWindow() == window;
        worker->cancel(); active = false; recognizing = false; dragging = false; ReleaseCapture(); ShowWindow(window, SW_HIDE);
        if (trackingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_CANCEL | TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking); trackingMouse = false;
        }
        SetWindowLongPtr(window, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        std::vector<Word>().swap(words); std::vector<WordRow>().swap(wordRows);
        std::vector<Span>().swap(selection); std::vector<Span>().swap(baseSelection);
        boxDamage.clear(); paintRegion.clear();
        fonts.clear(); highlights.clear(); frame.reset(); releaseCanvas();
        dragTrace.save();
        if (restoreFocus && IsWindow(previousWindow)) SetForegroundWindow(previousWindow);
    }
    void measure(Word& word, HDC dc, TextMetricsCache& cache) {
        int count = length(word); word.edges.resize(count + 1); word.edges[0] = word.rect.left;
        int height = std::max(8L, word.rect.bottom - word.rect.top);
        const auto& metrics = cache.get(height, word.text, dc, fonts);
        int width = word.rect.right - word.rect.left;
        for (int i = 1; i <= count; ++i) word.edges[i] = word.rect.left + (metrics.width > 0 ? MulDiv(metrics.advances[i - 1], width, metrics.width) : MulDiv(i, width, count));
        word.edges.back() = word.rect.right;
    }
    void accept(Result& r) {
        if (!active || r.id != session) return;
        try { acceptResult(r); }
        catch (...) {
            recognizing = false; pendingCopy = 0;
            invalidate();
            throw;
        }
    }
    void acceptResult(Result& r) {
        bool foundText = !r.words.empty();
        // Ctrl-drag can overlap a previous box. Select existing words again rather
        // than duplicating their text in the clipboard or remeasuring them.
        if (!words.empty()) {
            OcrPositionIndex existing;
            for (size_t i = 0; i < words.size(); ++i) existing.add(words[i].rect,i);
            r.words.erase(std::remove_if(r.words.begin(), r.words.end(), [&](const Word& incoming) {
                size_t first = words.size();
                existing.any(incoming.rect,[&](size_t i) {
                    if (words[i].monitor == incoming.monitor && sameOcrPosition(words[i].rect,incoming.rect)) first = std::min(first,i);
                    return false;
                });
                if (first == words.size()) return false;
                selection[first] = {0,length(words[first])}; return true;
            }), r.words.end());
        }
        int lineOffset = words.empty() ? 0 : words.back().line + 1;
        for (auto& word : r.words) word.line += lineOffset;
        // Use the existing render DC; it is session-owned and remains valid if
        // font/metrics allocation throws, unlike an acquired window DC.
        HDC dc = renderDc; TextMetricsCache metrics;
        size_t existing = words.size();
        // Prepare before committing: a failed font/metrics allocation must not
        // leave new words without matching selection spans and row indexes.
        for (auto& word : r.words) measure(word, dc, metrics);
        words.reserve(existing + r.words.size());
        selection.reserve(existing + r.words.size());
        wordRows.reserve(wordRows.size() + r.words.size());
        words.insert(words.end(), std::make_move_iterator(r.words.begin()), std::make_move_iterator(r.words.end()));
        // OCR boxes describe ink rather than line height: short letters, capitals, and
        // descenders have different bounds. Normalize nearby words into one padded
        // band, splitting wide gaps so unrelated columns never get connected.
        for (size_t first = existing; first < words.size();) {
            size_t end = first + 1;
            LONG top = words[first].rect.top, bottom = words[first].rect.bottom;
            while (end < words.size()) {
                const auto& previous = words[end - 1]; const auto& next = words[end];
                LONG height = std::max(previous.rect.bottom - previous.rect.top, next.rect.bottom - next.rect.top);
                if (next.line != previous.line || next.monitor != previous.monitor ||
                    next.rect.left - previous.rect.right > std::max(12L, height * 2)) break;
                top = std::min(top, next.rect.top); bottom = std::max(bottom, next.rect.bottom); ++end;
            }
            LONG padding = std::max(1L, (bottom - top) / 8);
            for (size_t i = first; i < end; ++i)
                words[i].highlightBand = {words[i].rect.left, top - padding, words[i].rect.right, bottom + padding};
            first = end;
        }
        for (size_t first = existing; first < words.size();) {
            size_t end = first + 1; RECT bounds = words[first].rect;
            while (end < words.size() && words[end].line == words[first].line && words[end].monitor == words[first].monitor) {
                UnionRect(&bounds, &bounds, &words[end].rect); ++end;
            }
            wordRows.push_back({first, end, bounds}); first = end;
        }
        selection.resize(words.size());
        for (size_t i = existing; i < words.size(); ++i) selection[i] = {0, length(words[i])};
        highlights.changed();
        elapsed = r.elapsed; detectedLanguage = r.language; recognizing = !r.complete;
        lastError = r.error;
        if (!r.error.empty()) status = L"Couldn't read this box. R to retry; right-click for details or language.";
        else if (r.complete && !foundText) status = L"No text found. Try a larger box or right-click to change language.";
        else status = detectedLanguage + L"  \x00b7  Ctrl+C to copy  \x00b7  Enter to copy and close";
        if (r.complete && pendingCopy) {
            int action = std::exchange(pendingCopy, 0);
            if (r.error.empty() && foundText && selectedWords()) { invalidate(); copy(action == 2 || action == 4, action >= 3); return; }
        }
        invalidate();
    }
    std::pair<int, int> hit(POINT p, bool nearest) {
        highlights.prepare(words, wordRows);
        int best = highlights.hit(p, nearest, words);
        if (best < 0) return {-1, 0};
        const auto& w = words[best]; int c = 0, end = length(w);
        while (c < end) {
            int mid = c + (end - c) / 2;
            if (p.x >= (w.edges[mid] + w.edges[mid + 1]) / 2) c = mid + 1;
            else end = mid;
        }
        if (c > 0 && c < length(w) && low(w.text[c]) && high(w.text[c - 1])) ++c;
        return {best, c};
    }
    void cancelRecognition() {
        if (recognizing) { invalidateOutline(selectionBox); invalidate(hud); }
        worker->cancel(); session = worker->generation; recognizing = false; pendingCopy = 0;
    }
    void beginRegion(POINT p, bool add) {
        if (!active || !frame) return;
        cancelRecognition();
        previousBox = selectionBox;
        previousAddRegion = addRegion;
        boxMode = true; addRegion = add || addNext; addNext = false; copied = false;
        dragOrigin = p; dragging = true; focused = 0;
        selectionBox = dragRegion(p, p, frame->image->width, frame->image->height);
        invalidateBoxChange(previousBox, selectionBox);
        status = L"Release to read this box. Esc cancels this drag.";
        latestPointer = p;
        if (dragTrace.enabled()) { latestInput = DragFrameClock::now(); latestMessageTime = static_cast<DWORD>(GetMessageTime()); }
        SetCapture(window); invalidate(hud);
        UpdateWindow(window);
        if (!presenter.active()) dragClock.start({p.x + frame->desktop.left, p.y + frame->desktop.top});
    }
    void beginTextSelection(POINT p, bool add) {
        cancelRecognition();
        copied = false; focused = 0;
        if (!add) std::fill(selection.begin(), selection.end(), Span{});
        baseSelection = selection; boxMode = false; dragOrigin = p;
        auto [index, character] = hit(p, true);
        if (index < 0) { invalidate(); return; }
        anchorWord = index; anchorCharacter = character; dragging = true; SetCapture(window);
        textEnd = {index, character};
        invalidate();
    }
    void mouseDown(POINT p, bool doubleClick) {
        if (!active || !frame) return;
        focused = 0;
        if (int control = controlAt(p)) {
            pressed = control; SetCapture(window); invalidate(hud); return;
        }
        if (inside(hud, p)) {
            movingHud = true; hudOffset = {p.x - hud.left, p.y - hud.top};
            SetCapture(window); return;
        }
        bool add = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (doubleClick && !words.empty()) {
            auto [index, character] = hit(p, false);
            if (index >= 0) {
                cancelRecognition();
                if (!add) std::fill(selection.begin(), selection.end(), Span{});
                selection[index] = {0, length(words[index])}; copied = false;
                status = L"Ctrl+C to copy  \x00b7  Enter to copy and close"; invalidate(); return;
            }
        }
        if (!(GetKeyState(VK_MENU) & 0x8000) || words.empty()) { beginRegion(p, add); return; }
        beginTextSelection(p, add);
    }
    void mouseMove(POINT p) {
        int over = controlAt(p);
        if (over != hovered) { hovered = over; invalidate(hud); }
        if (!trackingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            trackingMouse = TrackMouseEvent(&tracking) != FALSE;
        }
        if (movingHud) {
            RECT old = hud, work = monitorWork(p);
            LONG oldWidth = hud.right - hud.left, oldHeight = hud.bottom - hud.top;
            double scale = monitorScale(p);
            bool scaleChanged = scale != uiScale;
            if (scaleChanged) invalidateOutline(selectionBox);
            uiScale = scale;
            LONG w = std::min(static_cast<LONG>(ui(380)), work.right - work.left - ui(24)), h = ui(46);
            hudOffset = {MulDiv(hudOffset.x, w, std::max(1L, oldWidth)), MulDiv(hudOffset.y, h, std::max(1L, oldHeight))};
            LONG x = std::clamp(p.x - hudOffset.x, work.left, std::max(work.left, work.right - w));
            LONG y = std::clamp(p.y - hudOffset.y, work.top, std::max(work.top, work.bottom - h));
            hud = {x, y, x + w, y + h};
            if (scaleChanged) invalidateOutline(selectionBox);
            if (!EqualRect(&old, &hud)) { layoutButtons(); invalidate(old); invalidate(hud); }
            return;
        }
        if (!dragging) return;
        if (boxMode) {
            // WM_MOUSEMOVE can predate the cursor sampled by the last frame.
            // Never let that queued coordinate rewind the visible box.
            p = currentDragPointer(p);
            RECT old = selectionBox;
            selectionBox = dragRegion(dragOrigin, p, frame->image->width, frame->image->height);
            if (EqualRect(&old,&selectionBox)) return;
            latestPointer = p;
            if (dragTrace.enabled()) { latestInput = DragFrameClock::now(); latestMessageTime = static_cast<DWORD>(GetMessageTime()); }
            if (presenter.active() || dragClock.request({p.x + frame->desktop.left, p.y + frame->desktop.top})) {
                if (!boxPaintPending) pendingBoxFrom = old;
                boxPaintPending = true;
            } else {
                if (boxPaintPending) { old = pendingBoxFrom; boxPaintPending = false; }
                invalidateBoxChange(old, selectionBox);
            }
            return;
        }
        updateTextSelection(p);
    }
    void updateTextSelection(POINT p) {
        auto [index, character] = hit(p, true); if (index < 0) return;
        auto endpoint = std::pair{index, character};
        if (endpoint == textEnd) return;
        auto start = std::pair{anchorWord, anchorCharacter}, end = std::pair{index, character};
        if (start > end) std::swap(start, end);
        bool changed = false, countChanged = false;
        // Coalesce consecutive changes on one highlight band, retaining separate
        // rows in the Windows update region instead of one desktop-sized union.
        RECT dirty{};
        auto flush = [&] { if (!IsRectEmpty(&dirty)) invalidate(dirty); dirty = {}; };
        // Moving one endpoint only changes the words between its old and new
        // positions, including reversals across the anchor. Restore those from
        // the gesture's base, then apply the new range in place.
        for (int i = std::min(textEnd.first, index); i <= std::max(textEnd.first, index); ++i) {
            Span next = baseSelection[i];
            if (i >= start.first && i <= end.first) {
                Span range{i == start.first ? start.second : 0, i == end.first ? end.second : length(words[i])};
                if (range.begin < range.end) next = next.begin < next.end ?
                    Span{std::min(next.begin, range.begin), std::max(next.end, range.end)} : range;
            }
            auto old = selection[i];
            if (old.begin == next.begin && old.end == next.end) continue;
            countChanged |= (old.begin < old.end) != (next.begin < next.end);
            selection[i] = next;
            RECT area = words[i].highlightBand;
            // Include the neighboring space so connected highlights are restored
            // correctly when shrinking or reversing a selection.
            LONG gap = std::max(12L, (area.bottom - area.top) * 2);
            InflateRect(&area, gap + 2, 2);
            if (!IsRectEmpty(&dirty) && (dirty.top != area.top || dirty.bottom != area.bottom)) flush();
            if (IsRectEmpty(&dirty)) dirty = area; else UnionRect(&dirty, &dirty, &area);
            changed = true;
        }
        textEnd = endpoint;
        if (changed) { flush(); if (countChanged || copied) invalidate(hud); copied = false; }
    }
    void mouseUp(POINT p) {
        if (movingHud) { movingHud = false; ReleaseCapture(); return; }
        if (pressed) {
            int control = std::exchange(pressed, 0); ReleaseCapture(); invalidate(hud);
            if (control == controlAt(p)) invokeControl(control);
            return;
        }
        if (dragging) mouseMove(p);
        finishDrag(true); ReleaseCapture();
        if (active) UpdateWindow(window);
    }
    void invokeControl(int control) {
        if (control == 1 && canCopy()) copy(false);
        else if (control == 2) help();
        else if (control == 3) exitMode();
    }
    void finishDrag(bool recognize = false) {
        if (!dragging) return;
        applyPendingBoxDamage(); dragClock.stop();
        dragging = false;
        dragTrace.save();
        if (!boxMode) { status = L"Ctrl+C to copy  \x00b7  Enter to copy and close"; invalidate(hud); return; }
        if (!recognize || selectionBox.right - selectionBox.left < 3 || selectionBox.bottom - selectionBox.top < 3) {
            RECT old = selectionBox;
            selectionBox = previousBox;
            addRegion = previousAddRegion;
            status = words.empty() ? L"Drag a box around the text you need." : L"Selection kept. Ctrl+C to copy or draw another box.";
            invalidateBoxChange(old, selectionBox); invalidate(hud); return;
        }
        if (!addRegion) {
            words.clear(); wordRows.clear(); selection.clear(); baseSelection.clear();
            fonts.clear(); highlights.clear(); detectedLanguage.clear();
        }
        regionWordStart = words.size();
        recognizing = true; copied = false; lastError.clear(); elapsed = 0;
        status = L"Reading this box. You can press Enter to copy as soon as it's ready.";
        avoidSelection();
        session = ++worker->generation;
        worker->submit({session, frame, selectionBox, settings.language, Clock::now()});
        invalidate();
    }
    std::wstring selectedText() const {
        std::wstring output; int lastLine = -1, lastMonitor = -1;
        for (size_t i = 0; i < words.size(); ++i) {
            auto s = selection[i]; if (s.begin >= s.end) continue;
            auto& w = words[i];
            if (!output.empty()) {
                if (w.monitor != lastMonitor) output += L"\r\n\r\n";
                else if (w.line != lastLine) output += L"\r\n";
                else output += L" ";
            }
            output.append(w.text, s.begin, s.end - s.begin); lastLine = w.line; lastMonitor = w.monitor;
        }
        return output;
    }
    static std::wstring singleLine(std::wstring_view text) {
        std::wstring output; bool space = false;
        for (wchar_t c : text) {
            if (iswspace(c)) { space = !output.empty(); continue; }
            if (space) output += L' ';
            output += c; space = false;
        }
        return output;
    }
    void copy(bool exitAfter, bool oneLine = false) {
        exitAfter = exitAfter || settings.closeAfterCopy;
        if (recognizing) {
            // Preserve both options when a single-line copy also closes capture.
            pendingCopy = oneLine ? (exitAfter ? 4 : 3) : (exitAfter ? 2 : 1);
            status = exitAfter ? L"Will copy and close when ready. Esc cancels." : L"Will copy when ready. Esc cancels.";
            invalidate(hud); return;
        }
        auto text = selectedText();
        if (oneLine) text = singleLine(text);
        if (text.empty()) { status = L"Draw a box around text first."; invalidate(hud); return; }
        size_t bytes = (text.size() + 1) * sizeof(wchar_t); HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) { status = L"Could not allocate clipboard memory."; invalidate(hud); return; }
        auto pointer = GlobalLock(memory);
        if (!pointer) { GlobalFree(memory); status = L"Could not access clipboard memory."; invalidate(hud); return; }
        memcpy(pointer, text.c_str(), bytes); GlobalUnlock(memory);
        if (!OpenClipboard(window)) { GlobalFree(memory); status = L"Clipboard is busy. Press Ctrl+C again."; invalidate(hud); return; }
        bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory); CloseClipboard();
        if (!ok) { GlobalFree(memory); status = L"Windows could not write the clipboard."; invalidate(hud); return; }
        copied = true;
        status = std::to_wstring(text.size()) + (oneLine ? L" characters copied as one line. Esc to close." : L" characters copied. Ctrl-drag to add more; Esc to close.");
        if (exitAfter) exitMode(); else invalidate(hud);
    }
    static void fill(HDC dc, RECT r, COLORREF color) {
        SetDCBrushColor(dc,color); FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    static void stroke(HDC dc, RECT r, int width, COLORREF color) {
        fill(dc, {r.left, r.top, r.right, r.top + width}, color);
        fill(dc, {r.left, r.bottom - width, r.right, r.bottom}, color);
        fill(dc, {r.left, r.top, r.left + width, r.bottom}, color);
        fill(dc, {r.right - width, r.top, r.right, r.bottom}, color);
    }
    void hudText(HDC dc, const std::wstring& value, RECT r, int size, COLORREF color, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, int weight = FW_NORMAL) {
        auto previous = SelectObject(dc, fonts.get(size, weight)); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
        DrawText(dc, value.c_str(), static_cast<int>(value.size()), &r, flags | DT_NOPREFIX); SelectObject(dc, previous);
    }
    void renderHud(HDC destination) {
        // Render flat shapes into a small premultiplied surface; labels are
        // drawn directly onto the opaque destination below.
        struct DrawingRuntime {
            ULONG_PTR token{};
            DrawingRuntime() { Gdiplus::GdiplusStartupInput input; if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) throw std::runtime_error("Cannot initialize toolbar drawing"); }
            ~DrawingRuntime() { Gdiplus::GdiplusShutdown(token); }
        };
        static DrawingRuntime runtime;
        using namespace Gdiplus;
        int margin = 0, width = hud.right - hud.left, height = hud.bottom - hud.top;
        auto count = selectedWords();
        bool enabled = !recognizing && count != 0;
        // Center the icon and measured label as one unit, at device-pixel bounds.
        SIZE copyExtent{};
        auto oldFont = SelectObject(destination, fonts.get(ui(14), FW_MEDIUM));
        GetTextExtentPoint32(destination, L"Copy", 4, &copyExtent);
        SelectObject(destination, oldFont);
        int groupWidth = ui(14) + ui(7) + copyExtent.cx;
        int iconLeft = copyButton.left + (copyButton.right - copyButton.left - groupWidth) / 2;
        int iconTop = copyButton.top + (copyButton.bottom - copyButton.top - ui(14)) / 2;
        // Retain only the small alpha shapes. Text stays on the opaque target
        // for ClearType; moving or partially repainting the HUD reuses its shapes.
        std::array<int, 6> style{width, height, pressed, hovered, focused, enabled};
        if (!hudCanvas || !hudDc || hudCanvas->width != width || hudCanvas->height != height) {
            if (hudDc) { SelectObject(hudDc, previousHudCanvas); DeleteDC(hudDc); hudDc = nullptr; }
            hudCanvas = std::make_unique<::Bitmap>(width, height);
            hudDc = CreateCompatibleDC(destination);
            if (!hudDc) throw std::runtime_error("Cannot create toolbar render device");
            previousHudCanvas = SelectObject(hudDc, hudCanvas->handle); hudStyleScale = 0;
        }
        if (style != hudStyle || uiScale != hudStyleScale) {
            GdiFlush();
            memset(hudCanvas->pixels, 0, static_cast<size_t>(width) * height * 4);
            Gdiplus::Bitmap image(width, height, width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(hudCanvas->pixels));
            Graphics g(&image); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetPixelOffsetMode(PixelOffsetModeHalf);
            auto rounded = [&](RectF rect, float radius, Brush& brush, Pen* pen = nullptr) {
                GraphicsPath path; float d = radius * 2;
                path.AddArc(rect.X, rect.Y, d, d, 180, 90); path.AddArc(rect.GetRight()-d, rect.Y, d, d, 270, 90);
                path.AddArc(rect.GetRight()-d, rect.GetBottom()-d, d, d, 0, 90); path.AddArc(rect.X, rect.GetBottom()-d, d, d, 90, 90); path.CloseFigure();
                g.FillPath(&brush, &path); if (pen) g.DrawPath(pen, &path);
            };
            float m = static_cast<float>(margin), w = static_cast<float>(width), h = static_cast<float>(height);
            RectF panel(m, m, w-1, h-1);
            SolidBrush panelFill(Color(255, 29, 30, 33));
            Pen border(Color(255, 43, 44, 48), static_cast<float>(uiScale));
            rounded(panel, static_cast<float>(ui(10)), panelFill, &border);
            RectF button(m+copyButton.left-hud.left, m+copyButton.top-hud.top, static_cast<float>(copyButton.right-copyButton.left), static_cast<float>(copyButton.bottom-copyButton.top));
            Color buttonColor = enabled ? (pressed == 1 ? Color(255,220,220,223) : hovered == 1 ? Color(255,255,255,255) : Color(255,244,244,245)) : Color(255,48,50,54);
            SolidBrush buttonFill(buttonColor); Pen focus(Color(255,160,189,218), static_cast<float>(std::max(1, ui(1))));
            rounded(button, static_cast<float>(ui(7)), buttonFill, focused == 1 ? &focus : nullptr);
            Pen divider(Color(255, 60, 62, 68), static_cast<float>(std::max(1, ui(1))));
            // Balance the visible gap, rather than the larger close hit target.
            int closeGlyphLeft = (closeButton.left + closeButton.right) / 2 - ui(5);
            float dividerX = static_cast<float>((copyButton.right + closeGlyphLeft) / 2 - hud.left - ui(2));
            g.DrawLine(&divider, dividerX, static_cast<float>(ui(13)), dividerX, static_cast<float>(height-ui(13)));
            RectF close(m+closeButton.left-hud.left, m+closeButton.top-hud.top, static_cast<float>(closeButton.right-closeButton.left), static_cast<float>(closeButton.bottom-closeButton.top));
            if (hovered == 3 || focused == 3) { SolidBrush hover(pressed == 3 ? Color(255,59,60,65) : Color(255,43,44,49)); rounded(close, static_cast<float>(ui(5)), hover, focused == 3 ? &focus : nullptr); }
            Pen cross(Color(255,176,180,190), static_cast<float>(uiScale*1.4)); cross.SetStartCap(LineCapRound); cross.SetEndCap(LineCapRound);
            float cx = close.X+close.Width/2, cy = close.Y+close.Height/2, arm = static_cast<float>(ui(5));
            g.DrawLine(&cross,cx-arm,cy-arm,cx+arm,cy+arm); g.DrawLine(&cross,cx+arm,cy-arm,cx-arm,cy+arm);
            hudStyle = style; hudStyleScale = uiScale;
        }
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        AlphaBlend(destination, hud.left-margin, hud.top-margin, width, height, hudDc, 0, 0, width, height, blend);
        // Small orthogonal icon edges must land on whole device pixels. Draw
        // these directly, without fractional GDI+ strokes or alpha filtering.
        COLORREF iconInk = enabled ? RGB(32, 35, 41) : RGB(122, 125, 134);
        COLORREF iconBackground = !enabled ? RGB(48, 50, 54) : pressed == 1 ? RGB(220, 220, 223) :
            hovered == 1 ? RGB(255, 255, 255) : RGB(244, 244, 245);
        HPEN iconPen = CreatePen(PS_SOLID, std::max(1, ui(1)), iconInk);
        auto previousPen = SelectObject(destination, iconPen);
        auto previousBrush = SelectObject(destination, GetStockObject(DC_BRUSH));
        SetDCBrushColor(destination, iconBackground);
        int edge = ui(11), offset = ui(3), corner = ui(3);
        RoundRect(destination, iconLeft, iconTop, iconLeft + edge, iconTop + edge, corner, corner);
        RoundRect(destination, iconLeft + offset, iconTop + offset,
            iconLeft + offset + edge, iconTop + offset + edge, corner, corner);
        SelectObject(destination, previousBrush); SelectObject(destination, previousPen); DeleteObject(iconPen);
        // Draw labels last, on the opaque final canvas. Native ClearType keeps
        // small glyphs sharp; drawing onto a transparent alpha surface cannot
        // preserve the same subpixel coverage.
        std::wstring heading = recognizing ? (pendingCopy ? L"Reading, then copying..." : L"Reading text...") :
            copied ? L"Copied to clipboard" : addNext ? L"Select more text" : !lastError.empty() ? L"Couldn't read text" :
            count ? std::to_wstring(count) + (count == 1 ? L" word selected" : L" words selected") :
            !IsRectEmpty(&selectionBox) ? L"No text found" : L"Drag to select text";
        if (status.starts_with(L"Could not") || status.starts_with(L"Clipboard") || status.starts_with(L"Windows could not") || status.starts_with(L"Couldn't save")) heading = status;
        RECT title{hud.left + ui(20), hud.top, copyButton.left - ui(12), hud.bottom};
        constexpr UINT textFlags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
        if (count && !recognizing && !copied && !addNext && lastError.empty() && heading != status) {
            auto number = std::to_wstring(count); SIZE extent{};
            auto previousFont = SelectObject(destination, fonts.get(ui(14), FW_BOLD));
            GetTextExtentPoint32(destination, number.c_str(), static_cast<int>(number.size()), &extent);
            SelectObject(destination, previousFont);
            hudText(destination, number, title, ui(14), RGB(245, 246, 248), textFlags, FW_BOLD);
            title.left += extent.cx;
            hudText(destination, count == 1 ? L" word selected" : L" words selected", title, ui(14), RGB(188, 192, 202));
        } else hudText(destination, heading, title, ui(14), RGB(225, 227, 233));
        RECT label{iconLeft + ui(14) + ui(7), copyButton.top, copyButton.right - ui(6), copyButton.bottom};
        hudText(destination, L"Copy", label, ui(14), enabled ? RGB(25, 28, 33) : RGB(157, 161, 170), textFlags, FW_MEDIUM);
    }
    void paint() {
        // Exposure/HUD paints may happen before the scheduled drag frame. Fold
        // in its pending damage so logical and displayed box positions agree.
        applyPendingBoxDamage();
        bool traceFrame = active && dragging && boxMode && dragTrace.enabled();
        int64_t paintBegin = traceFrame ? DragFrameClock::now() : 0;
        // Preserve complex update regions: using only rcPaint would repaint the
        // whole box interior even when just its thin outline changed.
        constexpr DWORD maxRectangles = 256;
        alignas(RGNDATA) std::array<BYTE,sizeof(RGNDATAHEADER) + maxRectangles * sizeof(RECT)> storage;
        auto data = reinterpret_cast<RGNDATA*>(storage.data());
        HRGN update = paintRegion.get(); DWORD bytes = 0;
        if (update && GetUpdateRgn(window,update,FALSE) == COMPLEXREGION)
            bytes = GetRegionData(update,static_cast<DWORD>(storage.size()),data);
        PAINTSTRUCT ps{}; HDC screen = BeginPaint(window, &ps);
        struct PaintScope { HWND window; PAINTSTRUCT& paint; ~PaintScope() { EndPaint(window, &paint); } } scope{window, ps};
        if (!active || !frame || !canvas) return;
        if (presenter.active()) {
            HRGN pending = presentedDamage.get();
            if (pending) {
                if (bytes && bytes <= storage.size() && data->rdh.nCount <= maxRectangles) CombineRgn(pending, pending, update, RGN_OR);
                else {
                    // The bounding rectangle also safely covers simple regions
                    // and update regions too complex for the bounded array.
                    HRGN part = CreateRectRgnIndirect(&ps.rcPaint);
                    if (part) { CombineRgn(pending, pending, part, RGN_OR); DeleteObject(part); }
                }
                presentPending = true;
            } else fallbackToGdi();
            return;
        }
        DWORD rectangles = 0; uint64_t pixels = 0;
        if (bytes && bytes <= storage.size() && data->rdh.nCount <= maxRectangles) {
            auto parts = reinterpret_cast<const RECT*>(data->Buffer);
            for (DWORD i = 0; i < data->rdh.nCount; ++i) {
                render(screen,parts[i]);
                pixels += static_cast<uint64_t>(parts[i].right - parts[i].left) * (parts[i].bottom - parts[i].top);
            }
            rectangles = data->rdh.nCount;
        } else {
            render(screen,ps.rcPaint);
            pixels = static_cast<uint64_t>(ps.rcPaint.right - ps.rcPaint.left) * (ps.rcPaint.bottom - ps.rcPaint.top);
            rectangles = 1;
        }
        // The last tile's BitBlt must reach the window before we return to
        // waiting for input. Otherwise its GDI batch can linger until later work.
        GdiFlush();
        if (traceFrame) dragTrace.record(latestInput, paintBegin, GetTickCount() - latestMessageTime, rectangles, pixels, selectionBox, latestPointer, dragClock.scheduledQpc());
    }
    void render(HDC screen, RECT damage) {
        RECT bounds{0,0,frame->image->width,frame->image->height};
        if (!IntersectRect(&damage, &damage, &bounds)) return;
        highlights.prepare(words, wordRows);
        for (LONG y = damage.top; y < damage.bottom; y += canvas->height)
            for (LONG x = damage.left; x < damage.right; x += canvas->width)
                renderTile(screen,{x,y,std::min(x + canvas->width,damage.right),std::min(y + canvas->height,damage.bottom)});
    }
    void renderTile(HDC screen, RECT damage) {
        HDC dc = renderDc;
        POINT origin{damage.left,damage.top};
        // Finish previous GDI writes before accessing the DIB pixels directly.
        GdiFlush();
        RECT clear{};
        if (IntersectRect(&clear, &selectionBox, &damage)) {
            // Copy the clear interior once. Only dim the four disjoint pieces
            // around it; large boxes used to blend their entire interior twice.
            blendRect(*canvas, *frame->image, clear, 0, 0, origin);
            for (RECT part : {RECT{damage.left, damage.top, damage.right, clear.top},
                RECT{damage.left, clear.bottom, damage.right, damage.bottom},
                RECT{damage.left, clear.top, clear.left, clear.bottom},
                RECT{clear.right, clear.top, damage.right, clear.bottom}})
                if (!IsRectEmpty(&part)) blendRect(*canvas, *frame->image, part, 0x00080e16, 62, origin);
        } else blendRect(*canvas, *frame->image, damage, 0x00080e16, 62, origin);
        SelectClipRgn(dc,nullptr); SetViewportOrgEx(dc,-damage.left,-damage.top,nullptr);
        IntersectClipRect(dc, damage.left, damage.top, damage.right, damage.bottom);
        // The row index survives character-selection changes. Only words on
        // rows intersecting this tile need their connected strips evaluated.
        highlights.intersecting(damage, words, selection, [&](RECT clipped) {
            blendRect(*canvas, *frame->image, clipped, 0x00258cde, 76,origin);
        });
        if (!IsRectEmpty(&selectionBox)) {
            int unit = std::max(1, ui(1));
            RECT border = selectionBox; InflateRect(&border, unit * 3, unit * 3);
            stroke(dc, border, unit * 4, RGB(8, 17, 27));
            InflateRect(&border, -unit, -unit);
            stroke(dc, border, unit * 2, RGB(92, 219, 255));
        }
        RECT toolbarDamage = hud;
        if (hudVisible() && intersects(toolbarDamage, damage)) renderHud(dc);

        if (uploadingFrame) {
            GdiFlush(); // finish strokes/text before reading the DIB for upload
            presenter.upload(damage, *canvas);
        } else BitBlt(screen, damage.left, damage.top, damage.right - damage.left, damage.bottom - damage.top, dc, damage.left, damage.top, SRCCOPY);
    }
    void help() {
        MessageBox(window, (L"Press " + settings.hotkey + L" to capture screen text.\n\nCAPTURE\nDrag a box around text, then release.\nCtrl-drag adds another box. The toolbar can be dragged out of the way.\n\nSELECT\nAlt-drag refines text. Ctrl+Alt-drag adds a text range.\nDouble-click selects a word. Ctrl+A selects all recognized text.\n\nCOPY\nCtrl+C or the Copy button copies text.\nEnable Close capture after copying in Settings to close automatically.\nEnter always copies and closes. You can press it while text is being read.\nCtrl+Shift+C copies as one line.\n\nRECOVER\nR retries the last box. Backspace clears the selection.\nEsc cancels a drag; otherwise it closes capture.\nRight-click for actions and OCR language.\nTab focuses toolbar buttons; Space or Enter activates them.\nF1 opens this help.\n\nText recognition runs locally. Screenshots stay in memory until capture closes.").c_str(),
            L"Glyph shortcuts", MB_OK | MB_ICONINFORMATION);
    }
    void discardRegionResult() {
        if (!addRegion || regionWordStart >= words.size()) return;
        // Retry replaces the last added box. Otherwise position deduplication
        // keeps its old readings and silently discards corrected OCR results.
        words.resize(regionWordStart); selection.resize(regionWordStart); baseSelection.clear();
        wordRows.erase(std::remove_if(wordRows.begin(), wordRows.end(), [&](const WordRow& row) {
            return row.first >= regionWordStart;
        }), wordRows.end());
        highlights.changed();
    }
    void retry() {
        if (IsRectEmpty(&selectionBox)) return;
        cancelRecognition(); discardRegionResult(); previousBox = selectionBox;
        boxMode = true; dragging = true; finishDrag(true);
    }
    void clearSelection() {
        cancelRecognition();
        applyPendingBoxDamage(); dragClock.stop();
        dragging = movingHud = false; pressed = focused = 0; ReleaseCapture();
        words.clear(); wordRows.clear(); selection.clear(); baseSelection.clear();
        fonts.clear(); highlights.clear(); selectionBox = {}; previousBox = {};
        copied = false; addNext = false; lastError.clear(); detectedLanguage.clear();
        status = L"Selection cleared. Drag a new box around text."; invalidate();
    }
    HMENU languageMenu() const {
        HMENU result = CreatePopupMenu();
        AppendMenu(result, MF_STRING | (settings.language == L"auto" ? MF_CHECKED : 0), 100, L"Automatic (Windows languages)");
        for (size_t i = 0; i < languages.size(); ++i) {
            auto label = languages[i].second + L" (" + languages[i].first + L")";
            AppendMenu(result, MF_STRING | (settings.language == languages[i].first ? MF_CHECKED : 0), 101 + i, label.c_str());
        }
        return result;
    }
    void chooseLanguage(UINT command) {
        if (command < 100 || command > 100 + languages.size()) return;
        auto language = command == 100 ? L"auto" : languages[command - 101].first;
        if (!WritePrivateProfileString(L"Glyph", L"language", language.c_str(), config.c_str())) {
            if (active) { status = L"Couldn't save the language. Check that settings.ini is writable."; invalidate(hud); }
            else notify(L"Couldn't save the language setting.", true);
            return;
        }
        settings.language = language;
        if (active) retry();
    }
    void captureMenu() {
        if (!active) return;
        if (dragging) { finishDrag(); ReleaseCapture(); }
        HMENU popup = CreatePopupMenu();
        UINT copyFlags = MF_STRING | (canCopy() || recognizing ? 0 : MF_GRAYED);
        AppendMenu(popup, copyFlags, 1, L"Copy text\tCtrl+C");
        AppendMenu(popup, copyFlags, 2, L"Copy and close\tEnter");
        AppendMenu(popup, copyFlags, 3, L"Copy as one line\tCtrl+Shift+C");
        AppendMenu(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenu(popup, MF_STRING, 4, L"Add another box\tCtrl-drag");
        AppendMenu(popup, MF_STRING | (IsRectEmpty(&selectionBox) ? MF_GRAYED : 0), 5, L"Retry this box\tR");
        AppendMenu(popup, MF_STRING, 6, L"Clear selection\tBackspace");
        AppendMenu(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu()), L"Recognition language");
        if (!lastError.empty()) AppendMenu(popup, MF_STRING, 7, L"Error details...");
        AppendMenu(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenu(popup, MF_STRING, 8, L"Shortcuts\tF1");
        AppendMenu(popup, MF_STRING, 9, L"Close capture\tEsc");
        POINT p{}; GetCursorPos(&p);
        UINT command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
        DestroyMenu(popup);
        switch (command) {
        case 1: copy(false); break;
        case 2: copy(true); break;
        case 3: copy(false, true); break;
        case 4: addNext = true; status = L"Drag another box. Its text will be added to your selection."; invalidate(hud); break;
        case 5: retry(); break;
        case 6: clearSelection(); break;
        case 7: MessageBox(window, lastError.c_str(), L"Recognition error", MB_OK | MB_ICONINFORMATION); break;
        case 8: help(); break;
        case 9: exitMode(); break;
        default: chooseLanguage(command); break;
        }
    }
    bool startupEnabled() {
        HKEY key{}; if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
        auto result = RegQueryValueEx(key, APP_NAME, nullptr, nullptr, nullptr, nullptr); RegCloseKey(key); return result == ERROR_SUCCESS;
    }
    bool toggleStartup() {
        bool enabled = startupEnabled(); HKEY key{};
        if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) { notify(L"Could not update start at login.", true); return false; }
        LSTATUS result;
        if (enabled) result = RegDeleteValue(key, APP_NAME);
        else {
            // Package activation survives MSIX updates (the executable's installation path changes).
            UINT32 count = 0; LONG packaged = GetCurrentPackageFullName(&count, nullptr);
            std::wstring command;
            if (packaged == ERROR_INSUFFICIENT_BUFFER) {
                UINT32 n = 0; GetCurrentApplicationUserModelId(&n, nullptr); std::wstring id(n, L'\0');
                if (GetCurrentApplicationUserModelId(&n, id.data()) != ERROR_SUCCESS) { RegCloseKey(key); notify(L"Could not resolve package activation.", true); return false; }
                if (!id.empty() && !id.back()) id.pop_back();
                command = L"explorer.exe shell:AppsFolder\\" + id;
            } else {
                wchar_t path[32768]{}; GetModuleFileName(nullptr, path, 32768); command = L"\"" + std::wstring(path) + L"\"";
            }
            result = RegSetValueEx(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key); if (result != ERROR_SUCCESS) notify(L"Could not update start at login.", true);
        return result == ERROR_SUCCESS;
    }
    void menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenu(menu, MF_STRING, 10, (L"Select screen text    " + settings.hotkey).c_str());
        AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu()), L"OCR language");
        AppendMenu(menu, MF_STRING, 11, L"Settings...");
        AppendMenu(menu, MF_STRING, 12, L"Reload settings");
        AppendMenu(menu, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), 13, L"Start at login");
        AppendMenu(menu, MF_STRING, 14, L"Help");
        AppendMenu(menu, MF_SEPARATOR, 0, nullptr); AppendMenu(menu, MF_STRING, 15, L"Quit Glyph");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu); PostMessage(window, WM_NULL, 0, 0);
        switch (command) {
        case 10: activate(); break;
        case 11: showSettings(); break;
        case 12: loadSettings(); break;
        case 13: toggleStartup(); break;
        case 14: help(); break;
        case 15: shutdown(); break;
        default:
            chooseLanguage(command);
        }
    }
    void shutdown() {
        if (quitting) return; quitting = true; exitMode(); UnregisterHotKey(window, HOTKEY_ID);
        NOTIFYICONDATA n{sizeof(n)}; n.hWnd = window; n.uID = 1; Shell_NotifyIcon(NIM_DELETE, &n);
        if (worker) worker->stop();
        // The worker has stopped; drain owned message payloads before destroying its target window.
        MSG message{};
        while (PeekMessage(&message, window, WM_OCR, WM_ENGINE, PM_REMOVE)) {
            if (message.message == WM_OCR) delete reinterpret_cast<Result*>(message.lParam);
            if (message.message == WM_ENGINE) delete reinterpret_cast<EngineInfo*>(message.lParam);
        }
        DestroyWindow(window);
    }
    LRESULT dispatch(UINT message, WPARAM wp, LPARAM lp) {
        if (message == taskbarCreated) { tray(true); return 0; }
        switch (message) {
        case WM_HOTKEY: if (wp == HOTKEY_ID) activate(); return 0;
        case WM_TRAY:
            if (LOWORD(lp) == WM_CONTEXTMENU) menu();
            else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) activate(); return 0;
        case WM_OCR: { std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp)); accept(*result); return 0; }
        case WM_ENGINE: {
            std::unique_ptr<EngineInfo> info(reinterpret_cast<EngineInfo*>(lp)); languages = std::move(info->languages);
            if (!info->error.empty()) notify(L"OCR engine is unavailable. " + info->error, true); return 0;
        }
        case WM_PAINT: paint(); return 0;
        case WM_DRAG_FRAME: presentDragFrame(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: mouseDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, false); return 0;
        case WM_LBUTTONDBLCLK: mouseDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, true); return 0;
        case WM_MOUSEMOVE: mouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
        case WM_LBUTTONUP: mouseUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
        case WM_CAPTURECHANGED: movingHud = false; pressed = 0; finishDrag(); invalidate(hud); return 0;
        case WM_CANCELMODE: movingHud = false; pressed = 0; finishDrag(); ReleaseCapture(); return 0;
        case WM_MOUSELEAVE: trackingMouse = false; if (hovered) { hovered = 0; invalidate(hud); } return 0;
        case WM_CONTEXTMENU: captureMenu(); return 0;
        case WM_SETCURSOR:
            if (active && LOWORD(lp) == HTCLIENT) {
                POINT p{}; GetCursorPos(&p); ScreenToClient(window, &p);
                int control = controlAt(p);
                SetCursor(LoadCursor(nullptr, control ? (control == 1 && !canCopy() ? IDC_ARROW : IDC_HAND) :
                    hudVisible() && inside(hud, p) ? IDC_SIZEALL : (!words.empty() && (GetKeyState(VK_MENU) & 0x8000) ? IDC_IBEAM : IDC_CROSS)));
                return TRUE;
            }
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_F4) return DefWindowProc(window, message, wp, lp);
            [[fallthrough]];
        case WM_KEYDOWN:
            if (!active) break;
            if (wp == VK_ESCAPE) {
                if (dragging) { finishDrag(); ReleaseCapture(); }
                else exitMode();
            }
            else if (dragging || movingHud || pressed) return 0;
            else if (wp == VK_TAB) {
                int direction = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
                if (!focused) focused = direction > 0 ? 1 : 3;
                else focused = focused == 1 ? 3 : 1;
                if (focused == 1 && !canCopy()) focused = 3;
                invalidate(hud);
            }
            else if (wp == VK_RETURN) { if (focused) invokeControl(focused); else copy(true); }
            else if (wp == VK_SPACE && focused) invokeControl(focused);
            else if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'C') copy(false, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            else if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'A') {
                for (size_t i = 0; i < words.size(); ++i) selection[i] = {0, length(words[i])};
                copied = false; invalidate();
            }
            else if (wp == VK_F1) help();
            else if (wp == 'R') retry();
            else if (wp == VK_BACK) clearSelection();
            else if (wp == VK_APPS || (wp == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) captureMenu();
            return 0;
        case WM_DISPLAYCHANGE: if (active) { exitMode(); notify(L"Display layout changed. Press the shortcut to capture again."); } return 0;
        case WM_DPICHANGED: invalidate(); return 0;
        case WM_ACTIVATEAPP: if (!wp && active) exitMode(); return 0;
        case WM_CLOSE: if (active) exitMode(); else shutdown(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if (wp) shutdown(); return 0;
        }
        return DefWindowProc(window, message, wp, lp);
    }
};

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams); app->window = window;
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProc(window, message, wp, lp);
    try { return app->dispatch(message, wp, lp); }
    catch (winrt::hresult_error const& e) { app->status = L"Windows error: " + std::wstring(e.message()); app->invalidate(); }
    catch (const std::exception& e) { app->status = widen(e); app->invalidate(); }
    catch (...) { app->status = L"Unexpected error. Press Esc and try again."; app->invalidate(); }
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    bool captureNow = false; std::filesystem::path tracePath;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--capture") captureNow = true;
        else if (std::wstring_view(argv[i]) == L"--trace-drag" && i + 1 < argc) tracePath = argv[++i];
    }
    LocalFree(argv);
    HANDLE mutex = CreateMutex(nullptr, FALSE, L"Local\\Glyph.Desktop.Singleton");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindow(L"GlyphOverlay", nullptr);
        if (existing) PostMessage(existing, WM_HOTKEY, HOTKEY_ID, 0); CloseHandle(mutex); return 0;
    }
    int code = 0;
    try {
        App app;
        if (!tracePath.empty()) app.dragTrace.enable(std::move(tracePath));
        WNDCLASSEX cls{sizeof(cls)}; cls.style = CS_DBLCLKS; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
        cls.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1)); cls.hCursor = LoadCursor(nullptr, IDC_CROSS); cls.lpszClassName = L"GlyphOverlay";
        if (!RegisterClassEx(&cls)) throw std::runtime_error("Cannot register overlay window");
        HWND window = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("Cannot create overlay window");
        app.initialize(); if (captureNow) PostMessage(window, WM_HOTKEY, HOTKEY_ID, 0);
        MSG message{};
        for (;;) {
            if (HANDLE timer = app.pendingFrame()) {
                DWORD ready = MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (ready == WAIT_OBJECT_0) { SendMessage(window, WM_DRAG_FRAME, 0, 0); continue; }
                if (ready == WAIT_FAILED) { app.shutdown(); code = 1; break; }
                // A message wake can contain only sent/internal messages.
                // GetMessage could block again after dispatching those and
                // strand the armed timer; PeekMessage keeps this wait bounded.
                if (!PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) continue;
                if (message.message == WM_QUIT) break;
            } else {
                BOOL result = GetMessage(&message, nullptr, 0, 0);
                if (result <= 0) { if (result == -1) { app.shutdown(); code = 1; } break; }
            }
            TranslateMessage(&message); DispatchMessage(&message);
        }
    } catch (winrt::hresult_error const& e) { MessageBox(nullptr, e.message().c_str(), APP_NAME, MB_OK | MB_ICONERROR); code = 1; }
    catch (const std::exception& e) { MessageBox(nullptr, widen(e).c_str(), APP_NAME, MB_OK | MB_ICONERROR); code = 1; }
    CloseHandle(mutex); return code;
}
