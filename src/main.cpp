#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using Clock = std::chrono::steady_clock;
constexpr UINT WM_TRAY = WM_APP + 1, WM_OCR = WM_APP + 2, WM_ENGINE = WM_APP + 3;
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
class FontCache {
    std::vector<std::pair<int, HFONT>> fonts;
public:
    ~FontCache() { clear(); }
    HFONT get(int height) {
        for (auto [size, font] : fonts) if (size == height) return font;
        HFONT font = CreateFont(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) throw std::runtime_error("Cannot create selection font metrics");
        fonts.emplace_back(height, font); return font;
    }
    void clear() { for (auto [size, font] : fonts) DeleteObject(font); fonts.clear(); }
};
struct Settings { std::wstring hotkey = L"Ctrl+Alt+T", language = L"auto"; UINT modifiers = MOD_CONTROL | MOD_ALT, key = 'T'; };
struct Job { uint64_t id{}; std::shared_ptr<Frame> frame; std::wstring language; Clock::time_point started; };
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

// Keep native resolution. Windows converts the OCR input to Gray8; the overlay
// continues to use the original, full-color screenshot.
SoftwareBitmap tileBitmap(const Frame& frame, int x, int y, int w, int h) {
    SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);
    auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
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
    reference.Close(); buffer.Close(); return SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Gray8);
}

// Trim only complete, exactly uniform border rows, leaving generous context.
// No thresholding, scaling, or removal of nonuniform pixels is involved.
std::pair<int, int> uniformBorder(const Frame& frame, RECT monitor) {
    int width = monitor.right - monitor.left, height = monitor.bottom - monitor.top;
    int ox = monitor.left - frame.desktop.left, oy = monitor.top - frame.desktop.top;
    auto pixels = static_cast<const uint32_t*>(frame.image->pixels);
    auto uniform = [&](int y, uint32_t color) {
        auto row = pixels + static_cast<size_t>(oy + y) * frame.image->width + ox;
        for (int x = 0; x < width; ++x) if ((row[x] & 0xffffff) != color) return false;
        return true;
    };
    int top = 0, bottom = height;
    uint32_t first = pixels[static_cast<size_t>(oy) * frame.image->width + ox] & 0xffffff;
    while (top < height && uniform(top, first)) ++top;
    uint32_t last = pixels[static_cast<size_t>(oy + height - 1) * frame.image->width + ox] & 0xffffff;
    while (bottom > top && uniform(bottom - 1, last)) --bottom;
    constexpr int context = 48;
    top = std::max(0, top - context); bottom = std::min(height, std::max(bottom, top + context) + context);
    return {top, bottom};
}

// Native x64 includes SSE2. Blend four BGRA pixels per iteration directly into
// the existing canvas, avoiding full-screen GDI fills and AlphaBlend surfaces.
// Original screenshot pixels stay immutable for OCR and crisp selected text.
void blendRect(Bitmap& destination, const Bitmap& source, RECT rect, uint32_t color, int alpha) {
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
        LONG width = rect.right - rect.left, x = 0;
        for (; x + 4 <= width; x += 4) {
            __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + offset + x));
            __m128i result = _mm_packus_epi16(blend(_mm_unpacklo_epi8(pixels, zero)), blend(_mm_unpackhi_epi8(pixels, zero)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to + offset + x), result);
        }
        for (; x < width; ++x) {
            uint32_t pixel = from[offset + x], output = 0;
            for (int channel = 0; channel < 3; ++channel) {
                int shift = channel * 8;
                int value = ((pixel >> shift) & 255) * (255 - alpha) + ((color >> shift) & 255) * alpha + 128;
                output |= static_cast<uint32_t>((value + (value >> 8) + 1) >> 8) << shift;
            }
            to[offset + x] = output;
        }
    }
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

class Worker {
    HWND window;
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
        OcrEngine engine{nullptr}; std::wstring engineLanguage;
        auto info = std::make_unique<EngineInfo>();
        try {
            for (auto language : OcrEngine::AvailableRecognizerLanguages())
                info->languages.emplace_back(language.LanguageTag().c_str(), language.DisplayName().c_str());
            engine = createEngine(L"auto"); engineLanguage = L"auto";
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
                if (!engine || engineLanguage != job.language) { engine = createEngine(job.language); engineLanguage = job.language; }
                result->language = engine.RecognizerLanguage().LanguageTag().c_str();
                int limit = static_cast<int>(OcrEngine::MaxImageDimension());
                if (limit <= 128) throw std::runtime_error("Windows reported an invalid OCR image limit");
                int nextLine = 0, monitorIndex = 0;
                for (auto monitor : job.frame->monitors) {
                    std::vector<Word> words;
                    auto [cropTop, cropBottom] = uniformBorder(*job.frame, monitor);
                    int width = monitor.right - monitor.left, height = cropBottom - cropTop;
                    // Bound Windows OCR's temporary image memory, not the source
                    // resolution. Full-width strips preserve horizontal text context.
                    constexpr int pixelBudget = 5 * 1024 * 1024;
                    const int overlap = 80;
                    const int maximumHeight = std::min(limit, std::max(128, pixelBudget / std::min(limit, width)));
                    const int stripCount = std::max(1, (height - overlap + maximumHeight - overlap - 1) / (maximumHeight - overlap));
                    const int tileHeight = std::min(maximumHeight, (height + overlap * (stripCount - 1) + stripCount - 1) / stripCount);
                    const int stepX = limit - overlap, stepY = std::max(1, tileHeight - overlap);
                    for (int y = 0; y < height && generation == job.id; y += stepY) {
                        for (int x = 0; x < width && generation == job.id; x += stepX) {
                            int tw = std::min(limit, width - x), th = std::min(tileHeight, height - y);
                            int ox = monitor.left - job.frame->desktop.left + x, oy = monitor.top - job.frame->desktop.top + cropTop + y;
                            auto bitmap = tileBitmap(*job.frame, ox, oy, tw, th);
                            auto recognized = engine.RecognizeAsync(bitmap).get();
                            if (generation != job.id) break;
                            for (auto line : recognized.Lines()) {
                                for (auto item : line.Words()) {
                                    auto r = item.BoundingRect();
                                    Word w{item.Text().c_str(), {ox + static_cast<int>(std::floor(r.X)), oy + static_cast<int>(std::floor(r.Y)),
                                        ox + static_cast<int>(std::ceil(r.X + r.Width)), oy + static_cast<int>(std::ceil(r.Y + r.Height))}, nextLine, monitorIndex, {}};
                                    // Assign overlap to the tile containing the word's center, so every word appears once.
                                    double cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
                                    if ((x > 0 && cx < overlap / 2) || (y > 0 && cy < overlap / 2) ||
                                        (x + tw < width && cx >= tw - overlap / 2) || (y + th < height && cy >= th - overlap / 2)) continue;
                                    if (!w.text.empty() && w.rect.right > w.rect.left && w.rect.bottom > w.rect.top) words.push_back(std::move(w));
                                }
                                ++nextLine;
                            }
                            if (x + tw >= width) break;
                        }
                        if (y + std::min(tileHeight, height - y) >= height) break;
                    }
                    if (generation != job.id) break;
                    // Group fragments from tile seams into visual rows, then order words within each row.
                    std::stable_sort(words.begin(), words.end(), [](const Word& a, const Word& b) { return a.rect.top < b.rect.top; });
                    LONG tallest = 0;
                    for (const auto& word : words) tallest = std::max(tallest, word.rect.bottom - word.rect.top);
                    std::vector<std::vector<Word>> rows;
                    for (auto& w : words) {
                        auto row = rows.rbegin();
                        for (; row != rows.rend(); ++row) {
                            const auto& anchor = row->front().rect;
                            if (w.rect.top - anchor.top > tallest) { row = rows.rend(); break; }
                            double centerA = (anchor.top + anchor.bottom) / 2.0, centerB = (w.rect.top + w.rect.bottom) / 2.0;
                            if (std::abs(centerA - centerB) <= std::max(3.0, std::min(anchor.bottom - anchor.top, w.rect.bottom - w.rect.top) * .45)) break;
                        }
                        if (row == rows.rend()) rows.push_back({std::move(w)}); else row->push_back(std::move(w));
                    }
                    for (auto& row : rows) {
                        std::stable_sort(row.begin(), row.end(), [](const Word& a, const Word& b) { return a.rect.left < b.rect.left; });
                        int line = nextLine++;
                        for (auto& w : row) { w.line = line; result->words.push_back(std::move(w)); }
                    }
                    ++monitorIndex;
                    if (monitorIndex < static_cast<int>(job.frame->monitors.size()) && !result->words.empty() && generation == job.id) {
                        auto partial = std::make_unique<Result>(); partial->id = result->id; partial->language = result->language;
                        partial->words = std::move(result->words);
                        partial->elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job.started).count());
                        send(std::move(partial));
                    }
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
        engine = nullptr; winrt::uninit_apartment();
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
    std::vector<Span> selection, baseSelection, nextSelection;
    FontCache fonts;
    std::map<std::pair<int, std::wstring>, TextMetrics> textMetrics;
    std::vector<std::pair<std::wstring, std::wstring>> languages;
    std::unique_ptr<Bitmap> canvas;
    HDC renderDc{};
    HGDIOBJ previousCanvas{};
    HICON icon{};
    bool active{}, recognizing{}, dragging{}, boxMode{}, quitting{};
    bool hotkeyRegistered{};
    uint64_t session{};
    int anchorWord{-1}, anchorCharacter{}, elapsed{};
    double uiScale{1.0};
    POINT dragOrigin{};
    RECT hud{}, copyButton{}, closeButton{}, selectionBox{};
    HWND previousWindow{};
    std::wstring status, detectedLanguage;
    UINT taskbarCreated = RegisterWindowMessage(L"TaskbarCreated");

    ~App() { releaseCanvas(); if (icon) DestroyIcon(icon); }
    void releaseCanvas() {
        if (renderDc) { SelectObject(renderDc, previousCanvas); DeleteDC(renderDc); renderDc = nullptr; }
        canvas.reset();
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
    void loadSettings() {
        wchar_t value[256]{};
        Settings next;
        GetPrivateProfileString(L"Glyph", L"hotkey", L"Ctrl+Alt+T", value, 256, config.c_str()); next.hotkey = value;
        GetPrivateProfileString(L"Glyph", L"language", L"auto", value, 256, config.c_str()); next.language = value;
        if (!parseHotkey(next)) { notify(L"Invalid shortcut. Use Ctrl+Alt+T, Shift+F8, or another modifier/key combination. Keeping the current shortcut.", true); return; }
        bool changed = (!hotkeyRegistered || next.modifiers != settings.modifiers || next.key != settings.key);
        if (changed) {
            if (hotkeyRegistered) UnregisterHotKey(window, HOTKEY_ID);
            if (!RegisterHotKey(window, HOTKEY_ID, next.modifiers | MOD_NOREPEAT, next.key)) {
                hotkeyRegistered = RegisterHotKey(window, HOTKEY_ID, settings.modifiers | MOD_NOREPEAT, settings.key) != FALSE;
                notify(L"The requested shortcut is already used. Change hotkey in Settings, then choose Reload settings. OCR is also available from the tray.", true); return;
            }
            hotkeyRegistered = true;
        }
        settings = std::move(next); tray(false);
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
    void activate() {
        if (active) { exitMode(); return; }
        previousWindow = GetForegroundWindow(); auto started = Clock::now();
        try {
            frame = capture();
            canvas = std::make_unique<Bitmap>(frame->image->width, frame->image->height);
            renderDc = CreateCompatibleDC(nullptr);
            if (!renderDc) throw std::runtime_error("Cannot create overlay render device");
            previousCanvas = SelectObject(renderDc, canvas->handle);
        }
        catch (const std::exception& e) { frame.reset(); releaseCanvas(); notify(widen(e), true); return; }
        words.clear(); wordRows.clear(); selection.clear(); baseSelection.clear(); nextSelection.clear(); detectedLanguage.clear(); status = L"Recognizing screen text...";
        active = true; recognizing = true; dragging = false; anchorWord = -1;
        session = ++worker->generation;
        POINT cursor{}; GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(monitor, &mi);
        UINT dpiX = 96, dpiY = 96; GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY); uiScale = dpiX / 96.0;
        RECT work = mi.rcWork; OffsetRect(&work, -frame->desktop.left, -frame->desktop.top);
        int width = std::min(static_cast<LONG>(ui(860)), work.right - work.left - ui(24));
        hud = {work.left + (work.right - work.left - width) / 2, work.top + ui(18), work.left + (work.right - work.left + width) / 2, work.top + ui(104)};
        copyButton = {hud.right - ui(142), hud.top + ui(12), hud.right - ui(48), hud.top + ui(44)};
        closeButton = {hud.right - ui(40), hud.top + ui(12), hud.right - ui(10), hud.top + ui(44)};
        // Capture is complete: OCR can prepare its bitmap while the UI paints the
        // initial overlay, rather than waiting for the entire first paint.
        worker->submit({session, frame, settings.language, started});
        SetWindowLongPtr(window, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_APPWINDOW);
        SetWindowPos(window, HWND_TOPMOST, frame->desktop.left, frame->desktop.top, frame->image->width, frame->image->height, SWP_SHOWWINDOW);
        SetForegroundWindow(window); SetFocus(window); invalidate(); UpdateWindow(window);
    }
    void exitMode() {
        if (!active) return;
        bool restoreFocus = GetForegroundWindow() == window;
        worker->cancel(); active = false; dragging = false; ReleaseCapture(); ShowWindow(window, SW_HIDE);
        SetWindowLongPtr(window, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        words.clear(); words.shrink_to_fit(); wordRows.clear(); selection.clear(); baseSelection.clear(); nextSelection.clear(); fonts.clear(); textMetrics.clear(); frame.reset(); releaseCanvas();
        if (restoreFocus && IsWindow(previousWindow)) SetForegroundWindow(previousWindow);
    }
    void measure(Word& word, HDC dc) {
        int count = length(word); word.edges.resize(count + 1); word.edges[0] = word.rect.left;
        int height = std::max(8L, word.rect.bottom - word.rect.top);
        auto [entry, inserted] = textMetrics.try_emplace({height, word.text});
        auto& metrics = entry->second;
        if (inserted) {
            auto old = SelectObject(dc, fonts.get(height)); SIZE extent{};
            metrics.advances.resize(count);
            BOOL ok = GetTextExtentExPoint(dc, word.text.c_str(), count, 0, nullptr, metrics.advances.data(), &extent);
            SelectObject(dc, old); metrics.width = ok ? extent.cx : 0;
        }
        int width = word.rect.right - word.rect.left;
        for (int i = 1; i <= count; ++i) word.edges[i] = word.rect.left + (metrics.width > 0 ? MulDiv(metrics.advances[i - 1], width, metrics.width) : MulDiv(i, width, count));
        word.edges.back() = word.rect.right;
    }
    void accept(Result& r) {
        if (!active || r.id != session) return;
        // Each batch owns only its new monitor's words. Existing character
        // geometry and selections remain valid without copying or measuring again.
        HDC dc = GetDC(window);
        size_t existing = words.size();
        words.insert(words.end(), std::make_move_iterator(r.words.begin()), std::make_move_iterator(r.words.end()));
        for (size_t i = existing; i < words.size(); ++i) measure(words[i], dc);
        ReleaseDC(window, dc);
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
        selection.resize(words.size()); baseSelection.resize(words.size());
        elapsed = r.elapsed; detectedLanguage = r.language; recognizing = !r.complete;
        if (!r.error.empty()) status = L"OCR failed: " + r.error;
        else if (r.complete && words.empty()) status = L"No text found. Try another screen or OCR language.";
        else status = std::to_wstring(words.size()) + L" words  |  " + detectedLanguage + L"  |  " + std::to_wstring(elapsed) + L" ms" + (recognizing ? L"  |  Recognizing remaining screens..." : L"");
        invalidate();
    }
    std::pair<int, int> hit(POINT p, bool nearest) const {
        int best = -1; double bestDistance = 1e30;
        auto distance = [](RECT r, POINT p) {
            LONG dx = std::max({r.left - p.x, 0L, p.x - r.right}), dy = std::max({r.top - p.y, 0L, p.y - r.bottom});
            return static_cast<double>(dx) * dx + static_cast<double>(dy) * dy * 4;
        };
        for (const auto& row : wordRows) {
            if ((!nearest && !inside(row.bounds, p)) || distance(row.bounds, p) >= bestDistance) continue;
            for (int i = static_cast<int>(row.first); i < static_cast<int>(row.end); ++i) {
                const auto& r = words[i].rect;
                if (!nearest && !inside(r, p)) continue;
                double d = distance(r, p);
                if (d < bestDistance) { bestDistance = d; best = i; if (!d) break; }
            }
        }
        if (best < 0) return {-1, 0};
        const auto& w = words[best]; int c = 0;
        for (int i = 0; i < length(w); ++i) { if (p.x >= (w.edges[i] + w.edges[i + 1]) / 2) c = i + 1; }
        if (c > 0 && c < length(w) && low(w.text[c]) && high(w.text[c - 1])) ++c;
        return {best, c};
    }
    void mouseDown(POINT p, bool doubleClick) {
        if (inside(closeButton, p)) { exitMode(); return; }
        if (inside(copyButton, p)) { copy(false); return; }
        if (inside(hud, p) || words.empty()) return;
        bool add = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!add) std::fill(selection.begin(), selection.end(), Span{});
        baseSelection = selection; boxMode = (GetKeyState(VK_MENU) & 0x8000) != 0; dragOrigin = p;
        auto [index, character] = hit(p, !doubleClick);
        if (index < 0) { invalidate(); return; }
        if (doubleClick) { selection[index] = {0, length(words[index])}; invalidate(); return; }
        anchorWord = index; anchorCharacter = character; dragging = true; SetCapture(window);
        selectionBox = {p.x, p.y, p.x, p.y}; invalidate();
    }
    void mouseMove(POINT p) {
        if (!dragging) return;
        nextSelection = baseSelection; RECT oldBox = selectionBox;
        if (boxMode) {
            selectionBox = {std::min(p.x, dragOrigin.x), std::min(p.y, dragOrigin.y), std::max(p.x, dragOrigin.x), std::max(p.y, dragOrigin.y)};
            for (size_t i = 0; i < words.size(); ++i) if (intersects(selectionBox, words[i].rect)) nextSelection[i] = {0, length(words[i])};
        } else {
            auto [index, character] = hit(p, true); if (index < 0) return;
            auto start = std::pair{anchorWord, anchorCharacter}, end = std::pair{index, character};
            if (start > end) std::swap(start, end);
            for (int i = start.first; i <= end.first; ++i) {
                Span range{i == start.first ? start.second : 0, i == end.first ? end.second : length(words[i])};
                if (range.begin >= range.end) continue;
                if (nextSelection[i].begin < nextSelection[i].end) range = {std::min(nextSelection[i].begin, range.begin), std::max(nextSelection[i].end, range.end)};
                nextSelection[i] = range;
            }
        }
        RECT dirty{}; bool changed = false;
        auto include = [&](RECT r) { if (changed) UnionRect(&dirty, &dirty, &r); else { dirty = r; changed = true; } };
        for (size_t i = 0; i < words.size(); ++i) {
            if (selection[i].begin == nextSelection[i].begin && selection[i].end == nextSelection[i].end) continue;
            RECT area = words[i].highlightBand;
            // Include the neighboring space so connected highlights are restored
            // correctly when shrinking or reversing a selection.
            LONG gap = std::max(12L, (area.bottom - area.top) * 2);
            InflateRect(&area, gap + 2, 2); include(area);
        }
        if (boxMode && !EqualRect(&oldBox, &selectionBox)) {
            InflateRect(&oldBox, 2, 2); include(oldBox);
            RECT newBox = selectionBox; InflateRect(&newBox, 2, 2); include(newBox);
        }
        selection.swap(nextSelection);
        if (changed) invalidate(dirty);
    }
    void finishDrag() {
        if (!dragging) return;
        dragging = false;
        if (boxMode) { RECT area = selectionBox; InflateRect(&area, 2, 2); invalidate(area); }
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
            output += w.text.substr(s.begin, s.end - s.begin); lastLine = w.line; lastMonitor = w.monitor;
        }
        return output;
    }
    void copy(bool exitAfter) {
        auto text = selectedText();
        if (text.empty()) { status = L"Select text first. Drag, double-click a word, or press Ctrl+A."; invalidate(); return; }
        size_t bytes = (text.size() + 1) * sizeof(wchar_t); HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) { status = L"Could not allocate clipboard memory."; invalidate(); return; }
        auto pointer = GlobalLock(memory);
        if (!pointer) { GlobalFree(memory); status = L"Could not access clipboard memory."; invalidate(); return; }
        memcpy(pointer, text.c_str(), bytes); GlobalUnlock(memory);
        if (!OpenClipboard(window)) { GlobalFree(memory); status = L"Clipboard is busy. Press Ctrl+C again."; invalidate(); return; }
        bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory); CloseClipboard();
        if (!ok) { GlobalFree(memory); status = L"Windows could not write the clipboard."; invalidate(); return; }
        status = L"Copied " + std::to_wstring(text.size()) + L" characters. Keep selecting, or press Esc to return.";
        if (exitAfter) exitMode(); else invalidate();
    }
    static void fill(HDC dc, RECT r, COLORREF color) { HBRUSH b = CreateSolidBrush(color); FillRect(dc, &r, b); DeleteObject(b); }
    void hudText(HDC dc, const std::wstring& value, RECT r, int size, COLORREF color, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
        auto previous = SelectObject(dc, fonts.get(size)); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
        DrawText(dc, value.c_str(), static_cast<int>(value.size()), &r, flags | DT_NOPREFIX); SelectObject(dc, previous);
    }
    void paint() {
        PAINTSTRUCT ps{}; HDC screen = BeginPaint(window, &ps);
        if (!active || !frame || !canvas) { EndPaint(window, &ps); return; }
        render(screen, ps.rcPaint); EndPaint(window, &ps);
    }
    void render(HDC screen, RECT damage) {
        RECT bounds{0, 0, canvas->width, canvas->height};
        if (!IntersectRect(&damage, &damage, &bounds)) return;
        HDC dc = renderDc;
        // Finish previous GDI writes before accessing the DIB pixels directly.
        GdiFlush(); blendRect(*canvas, *frame->image, damage, 0x00080e16, 33);
        SelectClipRgn(dc, nullptr); IntersectClipRect(dc, damage.left, damage.top, damage.right, damage.bottom);
        // Tint the captured pixels instead of redrawing OCR text in a guessed font.
        // Merge selected neighboring words into continuous strips, including spaces.
        auto drawHighlight = [&](RECT strip) {
            RECT clipped{}; if (!IntersectRect(&clipped, &strip, &damage)) return;
            blendRect(*canvas, *frame->image, clipped, 0x00258cde, 76);
        };
        RECT strip{}; bool haveStrip = false; size_t previousIndex = 0;
        for (size_t i = 0; i < words.size(); ++i) {
            const auto& w = words[i]; auto s = selection[i];
            if (s.begin >= s.end) { if (haveStrip) drawHighlight(strip); haveStrip = false; continue; }
            RECT next{w.edges[s.begin], w.highlightBand.top, w.edges[s.end], w.highlightBand.bottom};
            if (s.begin == 0) --next.left;
            if (s.end == length(w)) ++next.right;
            bool connect = haveStrip && previousIndex + 1 == i && selection[previousIndex].end == length(words[previousIndex]) && s.begin == 0 &&
                words[previousIndex].line == w.line && words[previousIndex].monitor == w.monitor && strip.top == next.top && strip.bottom == next.bottom &&
                next.left - strip.right <= std::max(12L, (next.bottom - next.top) * 2);
            if (connect) strip.right = std::max(strip.right, next.right);
            else { if (haveStrip) drawHighlight(strip); strip = next; haveStrip = true; }
            previousIndex = i;
        }
        if (haveStrip) drawHighlight(strip);
        if (dragging && boxMode) {
            HPEN pen = CreatePen(PS_DOT, 1, RGB(84, 209, 255)); auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, selectionBox.left, selectionBox.top, selectionBox.right, selectionBox.bottom);
            SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
        }
        if (intersects(hud, damage)) {
        fill(dc, hud, RGB(17, 27, 40)); RECT stripe{hud.left, hud.top, hud.left + ui(4), hud.bottom}; fill(dc, stripe, RGB(66, 192, 245));
        RECT title{hud.left + ui(18), hud.top + ui(9), copyButton.left - ui(10), hud.top + ui(36)};
        hudText(dc, recognizing ? L"Glyph  /  OCR MODE  /  Recognizing..." : L"Glyph  /  OCR MODE  /  Select text", title, ui(19), RGB(242, 247, 255));
        fill(dc, copyButton, RGB(34, 106, 151)); hudText(dc, L"Copy", copyButton, ui(15), RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        hudText(dc, L"\x00d7", closeButton, ui(24), RGB(226, 235, 244), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT help{hud.left + ui(18), hud.top + ui(38), hud.right - ui(14), hud.top + ui(59)};
        hudText(dc, L"Drag: text   Alt-drag: box   Ctrl-drag: add   Ctrl+A: all   Ctrl+C: copy   Enter: copy & exit   Esc: exit", help, ui(13), RGB(166, 188, 210));
        RECT detail{hud.left + ui(18), hud.top + ui(60), hud.right - ui(14), hud.bottom - ui(5)};
        hudText(dc, status, detail, ui(13), RGB(107, 207, 237));
        }
        BitBlt(screen, damage.left, damage.top, damage.right - damage.left, damage.bottom - damage.top, dc, damage.left, damage.top, SRCCOPY);
    }
    bool startupEnabled() {
        HKEY key{}; if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
        auto result = RegQueryValueEx(key, APP_NAME, nullptr, nullptr, nullptr, nullptr); RegCloseKey(key); return result == ERROR_SUCCESS;
    }
    void toggleStartup() {
        bool enabled = startupEnabled(); HKEY key{};
        if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) { notify(L"Could not update start at login.", true); return; }
        LSTATUS result;
        if (enabled) result = RegDeleteValue(key, APP_NAME);
        else {
            // Package activation survives MSIX updates (the executable's installation path changes).
            UINT32 count = 0; LONG packaged = GetCurrentPackageFullName(&count, nullptr);
            std::wstring command;
            if (packaged == ERROR_INSUFFICIENT_BUFFER) {
                UINT32 n = 0; GetCurrentApplicationUserModelId(&n, nullptr); std::wstring id(n, L'\0');
                if (GetCurrentApplicationUserModelId(&n, id.data()) != ERROR_SUCCESS) { RegCloseKey(key); notify(L"Could not resolve package activation.", true); return; }
                if (!id.empty() && !id.back()) id.pop_back();
                command = L"explorer.exe shell:AppsFolder\\" + id;
            } else {
                wchar_t path[32768]{}; GetModuleFileName(nullptr, path, 32768); command = L"\"" + std::wstring(path) + L"\"";
            }
            result = RegSetValueEx(key, APP_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key); if (result != ERROR_SUCCESS) notify(L"Could not update start at login.", true);
    }
    void menu() {
        HMENU menu = CreatePopupMenu(), languageMenu = CreatePopupMenu();
        AppendMenu(menu, MF_STRING, 10, (L"Select screen text    " + settings.hotkey).c_str());
        AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(languageMenu, MF_STRING | (settings.language == L"auto" ? MF_CHECKED : 0), 100, L"Automatic (Windows languages)");
        for (size_t i = 0; i < languages.size(); ++i) {
            auto label = languages[i].second + L" (" + languages[i].first + L")";
            AppendMenu(languageMenu, MF_STRING | (settings.language == languages[i].first ? MF_CHECKED : 0), 101 + i, label.c_str());
        }
        AppendMenu(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu), L"OCR language");
        AppendMenu(menu, MF_STRING, 11, L"Settings (shortcut & language)");
        AppendMenu(menu, MF_STRING, 12, L"Reload settings");
        AppendMenu(menu, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), 13, L"Start at login");
        AppendMenu(menu, MF_STRING, 14, L"Help");
        AppendMenu(menu, MF_SEPARATOR, 0, nullptr); AppendMenu(menu, MF_STRING, 15, L"Quit Glyph");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(window);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
        DestroyMenu(menu); PostMessage(window, WM_NULL, 0, 0);
        switch (command) {
        case 10: activate(); break;
        case 11: ShellExecute(window, L"open", L"notepad.exe", (L"\"" + config.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); break;
        case 12: loadSettings(); break;
        case 13: toggleStartup(); break;
        case 14: MessageBox(window, (L"Press " + settings.hotkey + L" to freeze the screen and recognize text locally.\n\nDrag to select characters. Double-click selects a word.\nAlt-drag selects a box. Ctrl-drag adds another range.\nCtrl+A selects all. Ctrl+C copies. Enter copies and exits.\nEsc returns to the desktop.\n\nChange the shortcut or OCR language through Settings, then Reload settings.\nWindows OCR language features must be installed.\nScreenshots are kept in memory only during OCR mode.\n\nIf Windows blocks OCR, use the MSIX installation included with this app.").c_str(), APP_NAME, MB_OK | MB_ICONINFORMATION); break;
        case 15: shutdown(); break;
        default:
            if (command >= 100 && command <= 100 + languages.size()) {
                settings.language = command == 100 ? L"auto" : languages[command - 101].first;
                WritePrivateProfileString(L"Glyph", L"language", settings.language.c_str(), config.c_str());
            }
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
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: mouseDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, false); return 0;
        case WM_LBUTTONDBLCLK: mouseDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, true); return 0;
        case WM_MOUSEMOVE: mouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); return 0;
        case WM_LBUTTONUP: if (dragging) mouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); finishDrag(); ReleaseCapture(); return 0;
        case WM_CAPTURECHANGED: finishDrag(); return 0;
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
            if (!active) break;
            if (wp == VK_ESCAPE) exitMode();
            else if (wp == VK_RETURN) copy(true);
            else if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'C') copy(false);
            else if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'A') { for (size_t i = 0; i < words.size(); ++i) selection[i] = {0, length(words[i])}; invalidate(); }
            return 0;
        case WM_DISPLAYCHANGE: if (active) { exitMode(); notify(L"Display layout changed. Press the shortcut to capture again."); } return 0;
        case WM_DPICHANGED: invalidate(); return 0;
        case WM_CLOSE: shutdown(); return 0;
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
    bool captureNow = argc >= 2 && std::wstring(argv[1]) == L"--capture"; LocalFree(argv);
    HANDLE mutex = CreateMutex(nullptr, FALSE, L"Local\\Glyph.Desktop.Singleton");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindow(L"GlyphOverlay", nullptr);
        if (existing) PostMessage(existing, WM_HOTKEY, HOTKEY_ID, 0); CloseHandle(mutex); return 0;
    }
    int code = 0;
    try {
        App app;
        WNDCLASSEX cls{sizeof(cls)}; cls.style = CS_DBLCLKS; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
        cls.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1)); cls.hCursor = LoadCursor(nullptr, IDC_IBEAM); cls.lpszClassName = L"GlyphOverlay";
        if (!RegisterClassEx(&cls)) throw std::runtime_error("Cannot register overlay window");
        HWND window = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("Cannot create overlay window");
        app.initialize(); if (captureNow) PostMessage(window, WM_HOTKEY, HOTKEY_ID, 0);
        MSG message{}; BOOL result;
        while ((result = GetMessage(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessage(&message); }
        if (result == -1) { app.shutdown(); code = 1; }
    } catch (winrt::hresult_error const& e) { MessageBox(nullptr, e.message().c_str(), APP_NAME, MB_OK | MB_ICONERROR); code = 1; }
    catch (const std::exception& e) { MessageBox(nullptr, widen(e).c_str(), APP_NAME, MB_OK | MB_ICONERROR); code = 1; }
    CloseHandle(mutex); return code;
}
