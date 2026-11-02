// SPDX-License-Identifier: MIT
#include "backdrop.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "log.h"

namespace glasir {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClass[] = L"GlasirBackdrop";
constexpr UINT_PTR kCursorTimer = 1;  // also checks the stacking order
constexpr UINT_PTR kSyncTimer = 2;
constexpr UINT_PTR kSettleTimer = 3;
constexpr DWORD kCursorIdleMs = 1000;  // as mpv hides its own after a moment
constexpr UINT kSetMode = WM_APP + 1;  // thread message: wparam 1 = full screen, 0 = a window
constexpr UINT kPlaceAgain = WM_APP + 2;  // thread message: mpv left its own full screen
// After a switch, mpv's window is put in place again for this long if mpv
// (which changes its border on its own thread) moves it back meanwhile.
constexpr ULONGLONG kSettleMs = 1500;
constexpr LONG kWindowWidth = 1280, kWindowHeight = 720;  // as mpv's --geometry for a window (at 96 dpi)

// What the backdrop's thread works with; only that thread touches it.
struct State {
    HWND window = nullptr;
    HWND mpv = nullptr;
    WindowSetup setup;
    ComPtr<ITaskbarList2> taskbar;
    bool marked = false;  // the taskbar treats mpv's window as full screen
    // A switch under way: mpv's window goes to target (full screen: the
    // window; a window: its client area) once mpv has changed its border.
    bool settling = false;
    RECT target{};
    ULONGLONG settle_until = 0;
    POINT last_point{-1, -1};
    DWORD last_move = 0;
};
thread_local State *t_state = nullptr;

RECT monitor_rect(HWND hwnd) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

RECT work_rect(HWND hwnd) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

// A frame around the client area. Not the style: mpv's borderless window at
// start keeps WS_CAPTION and hides the frame itself (WM_NCCALCSIZE).
bool has_border(HWND hwnd) {
    RECT window{}, client{};
    if (!GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client)) return false;
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT *>(&client), 2);
    return !EqualRect(&window, &client);
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

// Full screen: where mpv's window goes on its monitor.
RECT full_screen_rect(const State &s) {
    const RECT mon = monitor_rect(s.mpv);
    return s.setup.skip_bars ? fit_picture(mon, s.setup.width, s.setup.height) : mon;
}

// Full screen without the bars, on a screen of another shape than the picture.
bool boxed(const State &s) {
    if (!s.setup.borderless || !s.setup.skip_bars) return false;
    const RECT mon = monitor_rect(s.mpv), picture = full_screen_rect(s);
    return !EqualRect(&mon, &picture);
}

void mark_full_screen(State &s, bool on) {
    if (!s.taskbar || s.marked == on || !IsWindow(s.mpv)) return;
    s.taskbar->MarkFullscreenWindow(s.mpv, on);
    s.marked = on;
}

// The backdrop goes right under mpv's window, over the whole monitor - or
// away while mpv is a window, minimised, hidden, gone or covers the screen
// itself.
void sync(State &s) {
    RECT r{};
    const bool show = boxed(s) && IsWindow(s.mpv) && IsWindowVisible(s.mpv) && !IsIconic(s.mpv) &&
                      !has_border(s.mpv) && GetWindowRect(s.mpv, &r);
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

// mpv's window goes to target (see State) for a moment, whatever mpv does
// meanwhile (it applies its own size and border on its own thread).
void settle_for(State &s, const RECT &target, ULONGLONG ms) {
    s.target = target;
    s.settling = true;
    s.settle_until = GetTickCount64() + ms;
    SetTimer(s.window, kSettleTimer, 30, nullptr);
}

// Full screen or a window. mpv changes its border itself (keeping its
// client area where it is); its window is then put in place by settle().
void switch_mode(State &s, bool borderless) {
    s.setup.borderless = borderless;
    if (s.setup.mpv_command) {
        s.setup.mpv_command({L"set", L"fullscreen", L"no"});
        s.setup.mpv_command({L"set", L"border", borderless ? L"no" : L"yes"});
    }
    if (borderless) {
        settle_for(s, full_screen_rect(s), kSettleMs);
    } else {
        const UINT dpi = GetDpiForWindow(s.mpv);
        settle_for(s,
                   centre_window(work_rect(s.mpv), MulDiv(kWindowWidth, dpi ? dpi : 96, 96),
                                 MulDiv(kWindowHeight, dpi ? dpi : 96, 96)),
                   kSettleMs);
    }
    mark_full_screen(s, boxed(s));
    sync(s);
    log(L"window: {} (the menu)", borderless ? (boxed(s) ? L"full screen, black around the picture" : L"full screen")
                                             : L"a window");
}

void settle(State &s) {
    if (GetTickCount64() > s.settle_until || !IsWindow(s.mpv)) {
        s.settling = false;
        KillTimer(s.window, kSettleTimer);
        return;
    }
    if (has_border(s.mpv) == s.setup.borderless) return;  // mpv has not changed its border yet
    RECT want = s.target;
    if (!s.setup.borderless) {
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(s.mpv, GWL_STYLE));
        const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(s.mpv, GWL_EXSTYLE));
        AdjustWindowRectExForDpi(&want, style, FALSE, ex, GetDpiForWindow(s.mpv));
    }
    RECT r{};
    if (GetWindowRect(s.mpv, &r) && !EqualRect(&r, &want))
        SetWindowPos(s.mpv, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    sync(s);
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
        } else if (wparam == kSettleTimer) {
            settle(*s);
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

RECT centre_window(const RECT &work, LONG width, LONG height) {
    const LONG ww = work.right - work.left, wh = work.bottom - work.top;
    if (width <= 0 || height <= 0 || ww <= 0 || wh <= 0) return work;
    // Room for the title bar and edges: at most 90 % of the work area.
    const double k = std::min({1.0, ww * 0.9 / width, wh * 0.9 / height});
    const LONG w = static_cast<LONG>(std::lround(width * k)), h = static_cast<LONG>(std::lround(height * k));
    const LONG x = work.left + (ww - w) / 2, y = work.top + (wh - h) / 2;
    return RECT{x, y, x + w, y + h};
}

void Backdrop::start(DWORD mpv_pid, WindowSetup setup) {
    stop();
    pid_ = mpv_pid;
    setup_ = std::move(setup);
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void Backdrop::set_borderless(bool borderless) {
    if (const DWORD id = thread_id_.load()) PostThreadMessageW(id, kSetMode, borderless ? 1 : 0, 0);
}

void Backdrop::place_again() {
    if (const DWORD id = thread_id_.load()) PostThreadMessageW(id, kPlaceAgain, 0, 0);
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
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // the queue exists before anything can post to it
    thread_id_ = GetCurrentThreadId();

    HWND mpv = nullptr;
    for (int i = 0; i < 600 && !mpv && !stopping_; ++i) {  // mpv opens its window within a second or two
        mpv = find_mpv(pid_);
        if (!mpv) Sleep(50);
    }
    if (!mpv || stopping_) return;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // already registered by an earlier session: fine
    State state;
    state.mpv = mpv;
    state.setup = setup_;
    const RECT mon = monitor_rect(mpv);
    state.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"Glasir", WS_POPUP, mon.left, mon.top,
                                   mon.right - mon.left, mon.bottom - mon.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!state.window) {
        log(L"window: no black backdrop ({}); full screen covers the screen and the menu cannot switch it",
            GetLastError());
        return;
    }
    t_state = &state;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&state.taskbar))) ||
        FAILED(state.taskbar->HrInit()))
        state.taskbar.Reset();
    if (boxed(state) && !setup_.fullscreen) {
        // mpv may still size its window itself (--geometry) just after it
        // shows it: put it back meanwhile.
        const RECT picture = full_screen_rect(state);
        SetWindowPos(mpv, nullptr, picture.left, picture.top, picture.right - picture.left,
                     picture.bottom - picture.top, SWP_NOZORDER | SWP_NOACTIVATE);
        settle_for(state, picture, 3000);
        // The taskbar steps back for mpv's window as it does for a full-screen one.
        mark_full_screen(state, true);
        log(L"window: the picture takes {}x{} of the {}x{} screen, black around it (DLSS 5 and frame generation "
            L"work on the picture only)",
            picture.right - picture.left, picture.bottom - picture.top, mon.right - mon.left, mon.bottom - mon.top);
    }
    sync(state);
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

    while (!stopping_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == kSetMode) {
            if (IsWindow(mpv)) switch_mode(state, msg.wParam != 0);
            continue;
        }
        if (!msg.hwnd && msg.message == kPlaceAgain) {
            if (IsWindow(mpv) && state.setup.borderless) {
                settle_for(state, full_screen_rect(state), kSettleMs);
                mark_full_screen(state, boxed(state));
                sync(state);
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (HWINEVENTHOOK hook : hooks)
        if (hook) UnhookWinEvent(hook);
    mark_full_screen(state, false);
    state.taskbar.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
    t_state = nullptr;
    DestroyWindow(state.window);
}

}  // namespace glasir
