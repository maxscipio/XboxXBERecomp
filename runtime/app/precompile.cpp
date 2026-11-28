// The shader pre-compile's window: the renderer compiles at launch the pipelines earlier runs used, when this driver
// does not have them yet (gpu/d3d12.cpp), and this shows how far it is. The window has a thread of its own and shows
// itself only once the work has taken a moment, so a launch with everything ready shows nothing.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

// the window's colours, as the first start's (game_code.cpp)
constexpr COLORREF kBackground = RGB(10, 11, 10);
constexpr COLORREF kText = RGB(227, 232, 219);
constexpr COLORREF kDim = RGB(145, 153, 138);
constexpr COLORREF kBarEmpty = RGB(28, 33, 26);
constexpr COLORREF kBarFull = RGB(145, 158, 130);

constexpr int64_t kShowAfterMs = 300;  // work done sooner than this never shows the window
constexpr UINT kCloseWindow = WM_APP + 1;

std::atomic<uint32_t> g_done{0};
uint32_t g_total = 0;
bool g_new_driver = false;
std::atomic<bool> g_skipped{false};
std::atomic<bool> g_dont_show{false};
std::atomic<HWND> g_window{nullptr};
std::thread g_thread;
int64_t g_opened = 0;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void DrawLine(HDC dc, RECT row, const std::wstring& text, COLORREF color, UINT align) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), int(text.size()), &row, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void FillBox(HDC dc, const RECT& box, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &box, brush);
    DeleteObject(brush);
}

// top to bottom: a heading, the count, the bar, then the don't show box and Skip
struct Layout {
    RECT heading;
    RECT count;
    RECT bar;
    RECT bottom;
    RECT skip;
    RECT dont_show;
};

Layout Arrange(const RECT& client, int unit) {
    Layout l;
    l.heading = {client.left + unit, client.top + unit, client.right - unit, client.top + unit * 3};
    l.count = l.heading;
    OffsetRect(&l.count, 0, unit * 2);
    l.bar = {l.count.left, l.count.bottom + unit / 2, l.count.right, l.count.bottom + unit / 2 + unit};
    l.bottom = {l.bar.left, l.bar.bottom + unit / 2, l.bar.right, l.bar.bottom + unit / 2 + unit * 2};
    l.skip = {l.bottom.right - unit * 8, l.bottom.top, l.bottom.right, l.bottom.bottom};
    l.dont_show = {l.bottom.left, l.bottom.top, l.skip.left - unit, l.bottom.bottom};
    return l;
}

void Paint(HDC dc, const RECT& client, int unit) {
    Layout l = Arrange(client, unit);
    FillBox(dc, client, kBackground);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* heading = g_new_driver ? L"The graphics driver changed: preparing the game's shaders again."
                                          : L"Preparing the game's shaders, so they do not stutter in the game.";
    DrawLine(dc, l.heading, heading, kText, DT_LEFT);
    uint32_t done = std::min(g_done.load(), g_total);
    float fraction = g_total ? float(done) / float(g_total) : 1.0f;
    std::wstring count = L"Shader " + std::to_wstring(done) + L" of " + std::to_wstring(g_total);
    DrawLine(dc, l.count, count, kDim, DT_LEFT);
    DrawLine(dc, l.count, std::to_wstring(int(fraction * 100.0f)) + L"%", kDim, DT_RIGHT);
    FillBox(dc, l.bar, kBarEmpty);
    RECT filled = l.bar;
    filled.right = l.bar.left + LONG(float(l.bar.right - l.bar.left) * fraction);
    FillBox(dc, filled, kBarFull);
    FillBox(dc, l.skip, kBarEmpty);
    DrawLine(dc, l.skip, L"Skip (B)", kText, DT_CENTER);
    int middle = (l.dont_show.top + l.dont_show.bottom) / 2;
    RECT box{l.dont_show.left, middle - unit / 2, l.dont_show.left + unit, middle + unit / 2};
    FillBox(dc, box, kBarEmpty);
    if (g_dont_show) InflateRect(&box, -unit / 4, -unit / 4);
    if (g_dont_show) FillBox(dc, box, kBarFull);
    RECT label = l.dont_show;
    label.left += unit * 3 / 2;
    DrawLine(dc, label, L"Don't show this again (X)", kText, DT_LEFT);
}

int Unit(HWND hwnd) { return MulDiv(10, GetDpiForWindow(hwnd), 96); }

// painted off screen, then copied, so the bar does not flicker
void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    HGDIOBJ old_font = SelectObject(memory, reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    Paint(memory, client, Unit(hwnd));
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_font);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

void Skip() {
    if (!g_skipped.exchange(true)) LOGR("Shaders: pre-compile skipped at %u of %u", g_done.load(), g_total);
}

void OnClick(HWND hwnd, LPARAM lp) {
    RECT client;
    GetClientRect(hwnd, &client);
    Layout l = Arrange(client, Unit(hwnd));
    POINT at{short(LOWORD(lp)), short(HIWORD(lp))};
    if (PtInRect(&l.skip, at)) Skip();
    else if (PtInRect(&l.dont_show, at)) g_dont_show = !g_dont_show;
}

// a controller's B skips and X ticks the box; on the press, not the hold
void PollController() {
    static uint16_t held = 0;
    uint16_t now = input::HeldOnAnyController();
    uint16_t pressed = now & ~held;
    held = now;
    if (pressed & XINPUT_GAMEPAD_X) g_dont_show = !g_dont_show;
    if (pressed & XINPUT_GAMEPAD_B) Skip();
}

// above the splash, which is up while the game boots
void ShowOnTop(HWND hwnd) {
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (!IsWindowVisible(hwnd) && NowMs() - g_opened >= kShowAfterMs) ShowOnTop(hwnd);
            if (IsWindowVisible(hwnd)) PollController();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            OnClick(hwnd, lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_SPACE) g_dont_show = !g_dont_show;
            if (wp == VK_ESCAPE) Skip();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE:  // the close button skips
            Skip();
            return 0;
        case kCloseWindow:
            DeleteObject(reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// centred on the primary monitor, sized for its DPI, in the message font; hidden until kShowAfterMs
HWND CreatePrecompileWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    std::wstring class_name = std::wstring(kGame.name) + L"Shaders";
    wc.lpszClassName = class_name.c_str();
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, MulDiv(520, dpi, 96), MulDiv(130, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kGame.title, style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(CreateFontIndirectW(&metrics.lfMessageFont)));
    SetTimer(hwnd, 1, 50, nullptr);  // shows it once due, reads the controller, repaints the count
    return hwnd;
}

}  // namespace

void OpenPrecompileWindow(uint32_t total, bool new_driver) {
    g_done = 0;
    g_total = total;
    g_new_driver = new_driver;
    g_skipped = false;
    g_dont_show = false;
    g_opened = NowMs();
    HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = std::thread([created] {
        g_window = CreatePrecompileWindow();
        SetEvent(created);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });
    WaitForSingleObject(created, INFINITE);
    CloseHandle(created);
}

void PrecompileStep() { g_done++; }

bool PrecompileSkipped() { return g_skipped; }

void ClosePrecompileWindow() {
    if (!g_thread.joinable()) return;
    PostMessageW(g_window, kCloseWindow, 0, 0);
    g_thread.join();
    g_window = nullptr;
    // ticked: later launches leave the shaders to the game, until the PC settings menu turns this on again
    if (g_dont_show && Get().precompile_shaders) {
        Get().precompile_shaders = false;
        Save();
        LOGR("Shaders: pre-compile turned off (don't show again)");
    }
}

}  // namespace app
