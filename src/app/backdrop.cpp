// SPDX-License-Identifier: MIT
#include "backdrop.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <cmath>
#include <cwchar>

#include "log.h"

namespace glasir {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClass[] = L"GlasirBackdrop";
constexpr UINT_PTR kCursorTimer = 1;  // also checks the stacking order
constexpr UINT_PTR kSyncTimer = 2;
constexpr DWORD kCursorIdleMs = 1000;  // as mpv hides its own after a moment

// What the backdrop's thread works with; only that thread touches it.
struct State {
    HWND window = nullptr;
    HWND mpv = nullptr;
    POINT last_point{-1, -1};
    DWORD last_move = 0;
};
thread_local State *t_state = nullptr;

RECT monitor_rect(HWND hwnd) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

// mpv's own window: visible, top-level and unowned, of that process.
HWND find_mpv(DWORD pid) {
    struct Find {
        DWORD pid;
        HWND found;
    } f{pid, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto *f = reinterpret_cast<Find *>(lp);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            RECT r{};
            if (!GetWindowRect(hwnd, &r) || r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;
            f->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// The backdrop goes right under mpv's window, over the whole monitor - or
// away while mpv is minimised, hidden, gone or covers the screen itself.
void sync(State &s) {
    RECT r{};
    const bool show = IsWindow(s.mpv) && IsWindowVisible(s.mpv) && !IsIconic(s.mpv) && GetWindowRect(s.mpv, &r);
    const RECT mon = show ? monitor_rect(s.mpv) : RECT{};
    if (!show || EqualRect(&r, &mon)) {
        if (IsWindowVisible(s.window)) ShowWindow(s.window, SW_HIDE);
        return;
    }
    SetWindowPos(s.window, s.mpv, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Nothing sits between mpv's window and the backdrop.
bool right_below(const State &s) {
    for (HWND h = GetWindow(s.mpv, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT))
        if (IsWindowVisible(h)) return h == s.window;
    return false;
}

void CALLBACK on_event(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    State *s = t_state;
    if (!s) return;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        // Said as a window is activated, before it is brought up: placed now,
        // the backdrop would go under mpv's old place, below other windows.
        sync(*s);
        SetTimer(s->window, kSyncTimer, 50, nullptr);
    } else if (hwnd == s->mpv && object == OBJID_WINDOW) {
        sync(*s);
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    State *s = t_state;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // the keyboard stays with mpv
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (s && IsWindow(s->mpv)) SetForegroundWindow(s->mpv);  // a click on the bars is a click on Glasir
        return 0;
    case WM_MOUSEMOVE:
        if (s) {
            const POINT p{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            if (p.x != s->last_point.x || p.y != s->last_point.y) {  // not a move Windows made up
                s->last_point = p;
                s->last_move = GetTickCount();
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            const bool idle = !s || GetTickCount() - s->last_move >= kCursorIdleMs;
            SetCursor(idle ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (!s) return 0;
        if (wparam == kSyncTimer) {
            KillTimer(hwnd, kSyncTimer);
            sync(*s);
        } else if (wparam == kCursorTimer) {
            if (GetForegroundWindow() == s->mpv && IsWindowVisible(hwnd) && !right_below(*s)) sync(*s);
            if (GetTickCount() - s->last_move >= kCursorIdleMs) {
                POINT p{};
                if (GetCursorPos(&p) && WindowFromPoint(p) == hwnd) SetCursor(nullptr);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

RECT fit_picture(const RECT &monitor, UINT32 width, UINT32 height) {
    const LONG mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
    if (width == 0 || height == 0 || mw <= 0 || mh <= 0) return monitor;
    LONG w = mw, h = static_cast<LONG>(std::lround(static_cast<double>(mw) * height / width));
    if (h > mh) {
        h = mh;
        w = static_cast<LONG>(std::lround(static_cast<double>(mh) * width / height));
    }
    if (mw - w <= 2 && mh - h <= 2) return monitor;  // the same shape, but for rounding
    const LONG x = monitor.left + (mw - w) / 2, y = monitor.top + (mh - h) / 2;
    return RECT{x, y, x + w, y + h};
}

void Backdrop::start(DWORD mpv_pid, UINT32 width, UINT32 height) {
    stop();
    pid_ = mpv_pid;
    width_ = width;
    height_ = height;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void Backdrop::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    if (const DWORD id = thread_id_.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    thread_.join();
    thread_id_ = 0;
}

void Backdrop::run() {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // the queue exists before stop() can post to it
    thread_id_ = GetCurrentThreadId();

    HWND mpv = nullptr;
    for (int i = 0; i < 600 && !mpv && !stopping_; ++i) {  // mpv opens its window within a second or two
        mpv = find_mpv(pid_);
        if (!mpv) Sleep(50);
    }
    if (!mpv || stopping_) return;
    const RECT mon = monitor_rect(mpv);
    const RECT picture = fit_picture(mon, width_, height_);
    if (EqualRect(&picture, &mon)) return;  // the picture has the screen's shape: nothing to do

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // already registered by an earlier session: fine
    State state;
    state.mpv = mpv;
    state.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"Glasir", WS_POPUP, mon.left, mon.top,
                                   mon.right - mon.left, mon.bottom - mon.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!state.window) {
        log(L"window: no black backdrop ({}); mpv stays full screen", GetLastError());
        return;
    }
    t_state = &state;

    SetWindowPos(mpv, nullptr, picture.left, picture.top, picture.right - picture.left, picture.bottom - picture.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    sync(state);
    // The taskbar steps back for mpv's window as it does for a full-screen one.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<ITaskbarList2> taskbar;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
        SUCCEEDED(taskbar->HrInit()))
        taskbar->MarkFullscreenWindow(mpv, TRUE);
    else
        taskbar.Reset();
    const HWINEVENTHOOK hooks[] = {
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, on_event, 0, 0,
                        WINEVENT_OUTOFCONTEXT),
        SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, on_event, pid_, 0,
                        WINEVENT_OUTOFCONTEXT),
        SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, on_event, pid_, 0, WINEVENT_OUTOFCONTEXT),
        SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, on_event, pid_, 0,
                        WINEVENT_OUTOFCONTEXT),
    };
    SetTimer(state.window, kCursorTimer, 250, nullptr);
    log(L"window: the picture takes {}x{} of the {}x{} screen, black around it (DLSS 5 and frame generation work on "
        L"the picture only)",
        picture.right - picture.left, picture.bottom - picture.top, mon.right - mon.left, mon.bottom - mon.top);

    while (!stopping_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (HWINEVENTHOOK hook : hooks)
        if (hook) UnhookWinEvent(hook);
    if (taskbar && IsWindow(mpv)) taskbar->MarkFullscreenWindow(mpv, FALSE);
    taskbar.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
    t_state = nullptr;
    DestroyWindow(state.window);
}

}  // namespace glasir
