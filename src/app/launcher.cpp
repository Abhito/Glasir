// SPDX-License-Identifier: MIT
#include "launcher.h"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "audio_endpoint.h"
#include "components.h"
#include "devices.h"
#include "glasir_version.h"
#include "gpu.h"
#include "log.h"
#include "settings.h"
#include "ui_style.h"
#include "update.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

// ---------------------------------------------------------------- what the PC has

struct Found {
    std::vector<CaptureDevice> devices;
    std::vector<std::wstring> directshow_only;
    std::vector<AudioInput> audio;
    GpuInfo gpu;
};

// Looked up on a thread of its own: opening every card to read its modes
// takes a moment, and the window should be there at once.
class Survey {
public:
    void start() {
        thread_ = std::thread([this] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Found f;
            f.devices = list_devices();
            for (const auto &name : list_directshow_devices()) {
                bool known = false;
                for (const auto &d : f.devices) known = known || names_match(d.name, name);
                if (!known) f.directshow_only.push_back(name);
            }
            f.audio = list_audio_inputs();
            f.gpu = display_gpu();
            if (SUCCEEDED(com)) CoUninitialize();
            std::lock_guard lock(m_);
            found_ = std::move(f);
            done_ = true;
        });
    }
    ~Survey() {
        if (thread_.joinable()) thread_.join();
    }
    bool done() const { return done_; }
    const Found &found() const { return found_; }  // once done

private:
    std::thread thread_;
    std::mutex m_;
    std::atomic<bool> done_{false};
    Found found_;
};

// ---------------------------------------------------------------- the choices

struct Choice {
    std::wstring device;  // "" = automatic
    UINT32 width = 0, height = 0;
    double fps = 0;
    bool hdr = false;
    bool dlss5 = true;
    bool framegen = true;
    bool fast = false;
    bool audio = true;
    std::wstring audio_device;
    bool borderless = true;
    bool show = true;
};

struct ModeItem {
    UINT32 width, height;
    double fps;
};

std::string utf8(const std::wstring &s) { return narrow(s); }

// 60 and 59.94 are different modes on most cards, so only a rate within a
// hundredth of a whole number (144.001) is shown as that number.
std::string fps_text(double fps) {
    const double r = std::round(fps);
    return std::abs(fps - r) < 0.01 ? std::format("{:.0f}", r) : std::format("{:.2f}", fps);
}

std::string mode_text(UINT32 w, UINT32 h, double fps) { return std::format("{} x {}  \xC2\xB7  {} fps", w, h, fps_text(fps)); }

// The device's modes Glasir can read in this picture mode, largest first.
std::vector<ModeItem> modes_for(const CaptureDevice *d, bool hdr) {
    std::vector<ModeItem> out;
    if (!d) return out;
    for (const auto &m : d->modes) {
        if (!m.supported || (m.format == PixelFormat::P010) != hdr) continue;
        bool dup = false;
        for (const auto &o : out) dup = dup || (o.width == m.width && o.height == m.height && std::abs(o.fps - m.fps) < 0.01);
        if (!dup) out.push_back({m.width, m.height, m.fps});
    }
    std::sort(out.begin(), out.end(), [](const ModeItem &a, const ModeItem &b) {
        if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
        return a.fps > b.fps;
    });
    return out;
}

// Whether the card also offers this mode in HDR (10-bit, P010).
bool has_hdr(const CaptureDevice *d, UINT32 w, UINT32 h, double fps) {
    if (!d) return false;
    for (const auto &m : d->modes)
        if (m.supported && m.format == PixelFormat::P010 && m.width == w && m.height == h && std::abs(m.fps - fps) < 0.01)
            return true;
    return false;
}

// A list item with "HDR" at its right end when hdr is set.
bool mode_item(const std::string &label, bool on, bool hdr) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::Selectable(label.c_str(), on);
    if (hdr) {
        const char *tag = "HDR";
        ImGui::SameLine(right - ImGui::CalcTextSize(tag).x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextColored(ImGui::GetStyleColorVec4(on || ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled), "%s", tag);
    }
    return clicked;
}

// ---------------------------------------------------------------- the window

struct Window {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    UINT resize_w = 0, resize_h = 0;
    float dpi_scale = 1;
    bool closed = false;
};
Window *g_window = nullptr;

void make_target(Window &w) {
    ComPtr<ID3D11Texture2D> back;
    w.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    w.device->CreateRenderTargetView(back.Get(), nullptr, &w.rtv);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    Window *w = g_window;
    switch (msg) {
    case WM_SIZE:
        if (w && wparam != SIZE_MINIMIZED) {
            w->resize_w = LOWORD(lparam);
            w->resize_h = HIWORD(lparam);
        }
        return 0;
    case WM_DPICHANGED:
        if (w) {
            w->dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT *r = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;  // no Alt menu
        break;
    case WM_CLOSE:
        if (w) w->closed = true;
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool open_window(Window &w) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"GlasirLauncher";
    RegisterClassExW(&wc);

    // Sized for the monitor the mouse is on, in its DPI.
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    UINT dpi = 96;
    if (HWND desktop = GetDesktopWindow()) dpi = GetDpiForWindow(desktop);
    using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            UINT x = 96, y = 96;
            if (SUCCEEDED(fn(monitor, 0, &x, &y))) dpi = x;
        }
    }
    w.dpi_scale = dpi / 96.0f;
    const int width = int(560 * w.dpi_scale), height = int(760 * w.dpi_scale);
    const RECT &work = mi.rcWork;
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + std::max(0L, (work.bottom - work.top - height) / 2);
    w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Glasir", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             x, y, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w.hwnd) return false;
    const BOOL dark = TRUE;  // a dark title bar to match
    DwmSetWindowAttribute(w.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &w.swapchain, &w.device, nullptr, &w.context);
    if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: the software one will do for a window
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &w.swapchain, &w.device, nullptr, &w.context);
    if (FAILED(hr)) {
        log(L"launcher: no Direct3D 11 ({})", hresult_text(hr));
        DestroyWindow(w.hwnd);
        return false;
    }
    make_target(w);
    ShowWindow(w.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(w.hwnd);
    SetForegroundWindow(w.hwnd);
    return true;
}

// ---------------------------------------------------------------- drawing

void heading(const char *text) {
    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
    ImGuiIO &io = ImGui::GetIO();
    ImGui::PushFont(io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr, ImGui::GetStyle().FontSizeBase * 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
}

void dim(const std::string &text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kTextDim);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// A label in the left column and the widget's start.
void row(const char *label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
    ImGui::SetNextItemWidth(-1);
}

// "Save a bug report", on a thread of its own (it takes a second or two).
struct BugReport {
    enum State { Idle, Saving, Saved, Failed };
    std::atomic<int> state{Idle};
    std::wstring error;  // Failed (set before state)
    std::thread thread;
    ~BugReport() {
        if (thread.joinable()) thread.join();
    }
    void start(const std::function<bool(std::wstring &, std::wstring &)> &save) {
        if (state == Saving) return;
        if (thread.joinable()) thread.join();
        state = Saving;
        thread = std::thread([this, save] {
            std::wstring zip, e;
            if (save(zip, e)) {
                // Explorer shows it, chosen, ready to be dragged into a message.
                const std::wstring select = L"/select,\"" + zip + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
                state = Saved;
            } else {
                error = e;
                state = Failed;
            }
        });
    }
};

struct Screen {
    Choice c;
    const Choice original;
    const Found *found = nullptr;
    bool dlss5_installed = false;
    bool dlss5_forced = false;  // glasir.ini: [dlss5] enabled = on (try without an RTX 50)
    std::string versions;  // "mpv 20261031  ·  ReShade 6.8.0  ·  DLSS 5 10.8.0", "" if none
    const LauncherExtras *extras = nullptr;
    Release release;  // the newer Glasir, once the update check found one
    BugReport *bug = nullptr;
    float content_height = 0;  // what the window needs, in pixels
    LauncherAction action = LauncherAction::Quit;
    bool finished = false;
};

void draw(Screen &s) {
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushFont(io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
    ImGui::TextUnformatted("Glasir");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    dim(std::string("  ") + GLASIR_VERSION);
    dim("Modern PC graphics for your console, through a capture card.");

    Choice &c = s.c;
    const Found *f = s.found;
    heading("CAPTURE");
    if (!f) {
        dim("Looking for capture cards...");
    } else {
        ModeRequest request;
        request.hdr = c.hdr;
        request.max_auto_height = UINT32(GetSystemMetrics(SM_CYSCREEN));
        const CaptureDevice *automatic = choose_device(f->devices, L"", request);
        const CaptureDevice *device = c.device.empty() ? automatic : nullptr;
        for (const auto &d : f->devices)
            if (!c.device.empty() && names_match(d.name, c.device)) device = &d;
        bool directshow = false;
        for (const auto &n : f->directshow_only) directshow = directshow || (!c.device.empty() && names_match(n, c.device));

        row("Card");
        const std::string auto_label =
            automatic ? "Automatic (" + utf8(automatic->name) + ")" : std::string("Automatic (none found yet)");
        const std::string current = c.device.empty() ? auto_label : utf8(c.device) + (directshow ? "  (DirectShow)" : "");
        if (ImGui::BeginCombo("##card", current.c_str())) {
            if (ImGui::Selectable(auto_label.c_str(), c.device.empty())) c.device.clear();
            // Items are told apart by their place, not their text: two cards
            // can share a name, and ImGui warns about matching IDs.
            int id = 0;
            for (const auto &d : f->devices) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(d.name).c_str(), names_match(d.name, c.device))) c.device = d.name;
                ImGui::PopID();
            }
            for (const auto &n : f->directshow_only) {
                ImGui::PushID(id++);
                if (ImGui::Selectable((utf8(n) + "  (DirectShow)").c_str(), names_match(n, c.device))) c.device = n;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (directshow) dim("This card works only through DirectShow: no dedup, so no frame generation.");
        if (f->devices.empty() && f->directshow_only.empty()) dim("No capture card found. Plug it in and start Glasir again.");

        row("Picture");
        if (ImGui::RadioButton("SDR", !c.hdr)) c.hdr = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("HDR", c.hdr)) c.hdr = true;
        dim(c.hdr ? "HDR needs HDR on the console. Only the card's HDR (10-bit) modes are listed."
                  : "Modes marked HDR can also be played in HDR.");

        const auto modes = modes_for(device, c.hdr);
        bool mode_ok = c.width == 0;
        for (const auto &m : modes)
            mode_ok = mode_ok || (m.width == c.width && m.height == c.height && std::abs(m.fps - c.fps) < 0.01);
        if (!mode_ok && device) c.width = c.height = 0, c.fps = 0;  // not offered here: back to automatic
        request.width = request.height = 0;
        request.fps = 0;
        const auto picked = device ? choose_mode(device->modes, request) : std::nullopt;
        const std::string auto_mode =
            picked ? "Automatic (" + mode_text(picked->width, picked->height, picked->fps) + ")" : std::string("Automatic");
        row("Mode");
        const std::string mode_label = c.width ? mode_text(c.width, c.height, c.fps) : auto_mode;
        if (ImGui::BeginCombo("##mode", mode_label.c_str())) {
            const bool auto_hdr = !c.hdr && picked && has_hdr(device, picked->width, picked->height, picked->fps);
            if (mode_item(auto_mode, c.width == 0, auto_hdr)) c.width = c.height = 0, c.fps = 0;
            int id = 0;
            for (const auto &m : modes) {
                const bool on = m.width == c.width && m.height == c.height && std::abs(m.fps - c.fps) < 0.01;
                ImGui::PushID(id++);
                if (mode_item(mode_text(m.width, m.height, m.fps), on, !c.hdr && has_hdr(device, m.width, m.height, m.fps))) {
                    c.width = m.width;
                    c.height = m.height;
                    c.fps = m.fps;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (device && modes.empty()) dim(c.hdr ? "This card has no HDR mode Glasir can read." : "This card has no mode Glasir can read; mpv opens it through DirectShow.");
    }

    heading("ENHANCEMENTS");
    const bool rtx50 = f && is_rtx50(f->gpu);
    const bool forced = s.dlss5_forced;
    if (!s.dlss5_installed || !(rtx50 || forced)) ImGui::BeginDisabled();
    bool dlss5 = c.dlss5 && s.dlss5_installed && (rtx50 || forced);
    if (ImGui::Checkbox("DLSS 5 neural rendering", &dlss5)) c.dlss5 = dlss5;
    if (!s.dlss5_installed || !(rtx50 || forced)) ImGui::EndDisabled();
    if (f && !rtx50 && !forced)
        dim("Needs an NVIDIA RTX 50 series GPU" + (f->gpu.found ? " (this PC: " + utf8(f->gpu.name) + ")." : std::string(".")));
    else if (f && !s.dlss5_installed) {
        dim("Not downloaded yet (about 250 MB).");
        if (ImGui::Button("Download DLSS 5")) {
            s.action = LauncherAction::Setup;
            s.finished = true;
        }
    }
    ImGui::Checkbox("Frame generation (doubles the frame rate)", &c.framegen);
    if (c.framegen) {
        ImGui::Indent();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Quality");
        ImGui::SameLine();
        if (ImGui::RadioButton("High", !c.fast)) c.fast = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Fast (less GPU)", c.fast)) c.fast = true;
        ImGui::Unindent();
    }

    heading("SOUND");
    ImGui::Checkbox("Play the card's sound", &c.audio);
    if (c.audio && f) {
        row("Input");
        const std::string own = "The card's own (automatic)";
        const std::string current = c.audio_device.empty() ? own : utf8(c.audio_device);
        if (ImGui::BeginCombo("##audio", current.c_str())) {
            if (ImGui::Selectable(own.c_str(), c.audio_device.empty())) c.audio_device.clear();
            int id = 0;
            for (const auto &a : f->audio) {
                ImGui::PushID(id++);
                if (ImGui::Selectable(utf8(a.name).c_str(), a.name == c.audio_device)) c.audio_device = a.name;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    heading("WINDOW");
    row("Show as");
    if (ImGui::BeginCombo("##window", c.borderless ? "Full screen (borderless)" : "A window")) {
        if (ImGui::Selectable("Full screen (borderless)", c.borderless)) c.borderless = true;
        if (ImGui::Selectable("A window", !c.borderless)) c.borderless = false;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Show this window when Glasir starts", &c.show);
    if (!c.show) dim("Glasir then plays at once. Hold Shift while starting it to see this window.");

    ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
    const float button_h = ImGui::GetFrameHeight() * 1.8f;
    ImGui::PushStyleColor(ImGuiCol_Button, ui::kGold);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.80f, 0.36f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.58f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.05f, 1.0f));
    ImGui::PushFont(io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    if (ImGui::Button("Play", ImVec2(-1, button_h))) {
        s.action = LauncherAction::Play;
        s.finished = true;
    }
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    dim("While playing, Tab opens the menu.");
    ImGui::AlignTextToFramePadding();
    dim(s.versions.empty() ? "mpv and ReShade are downloaded when you press Play." : s.versions);
    if (!s.versions.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Update")) {
            s.action = LauncherAction::Setup;
            s.finished = true;
        }
    }
    if (s.extras && s.extras->update && s.extras->update->state() == UpdateCheck::State::Available) {
        if (s.release.tag.empty()) s.release = s.extras->update->release();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kGold);
        ImGui::TextUnformatted(("Glasir " + narrow(version_text(s.release.version)) + " is out.").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("Update Glasir")) {
            s.action = LauncherAction::SelfUpdate;
            s.finished = true;
        }
        if (!s.release.page.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("What's new"))
                ShellExecuteW(nullptr, L"open", s.release.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    if (s.extras && s.extras->bug_report && s.bug) {
        const int state = s.bug->state.load();
        ImGui::AlignTextToFramePadding();
        if (state == BugReport::Saving) {
            dim("Saving a bug report...");
        } else {
            if (ImGui::SmallButton("Save a bug report")) s.bug->start(s.extras->bug_report);
            ImGui::SameLine();
            if (state == BugReport::Saved)
                dim("Saved in \"Bug reports\" (logs, settings and the PC; no user name).");
            else if (state == BugReport::Failed)
                dim("Could not save it: " + narrow(s.bug->error));
            else
                dim("F9 while playing adds the frames on screen.");
        }
    }
    s.content_height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::End();
}

// What changed goes into glasir.ini; what did not stays as the user wrote it.
void save(const std::wstring &path, const Choice &c, const Choice &o, bool rtx50) {
    std::vector<SettingValue> v;
    auto num = [](double d) { return d == std::floor(d) ? std::format(L"{:.0f}", d) : std::format(L"{:.3f}", d); };
    if (c.device != o.device) v.push_back({L"capture", L"device", c.device});
    if (c.width != o.width || c.height != o.height || c.fps != o.fps) {
        v.push_back({L"capture", L"width", std::to_wstring(c.width)});
        v.push_back({L"capture", L"height", std::to_wstring(c.height)});
        v.push_back({L"capture", L"fps", num(c.fps)});
    }
    if (c.hdr != o.hdr) v.push_back({L"video", L"mode", c.hdr ? L"hdr" : L"sdr"});
    if (c.dlss5 != o.dlss5) v.push_back({L"dlss5", L"enabled", c.dlss5 ? (rtx50 ? L"auto" : L"on") : L"off"});
    if (c.framegen != o.framegen) v.push_back({L"framegen", L"enabled", c.framegen ? L"1" : L"0"});
    if (c.fast != o.fast) v.push_back({L"framegen", L"quality", c.fast ? L"fast" : L"high"});
    if (c.audio != o.audio) v.push_back({L"audio", L"enabled", c.audio ? L"1" : L"0"});
    if (c.audio_device != o.audio_device) v.push_back({L"audio", L"device", c.audio_device});
    if (c.borderless != o.borderless) v.push_back({L"window", L"mode", c.borderless ? L"borderless" : L"windowed"});
    if (c.show != o.show) v.push_back({L"launcher", L"show", c.show ? L"1" : L"0"});
    if (v.empty()) return;
    save_settings(path, v);
    log(L"launcher: {} setting(s) saved to {}", v.size(), path);
}

// What engine\components.ini says is installed, in a line.
std::string versions_line(const std::wstring &engine_dir) {
    const std::wstring ini = engine_dir + L"\\components.ini";
    auto get = [&](const wchar_t *section, const wchar_t *key) {
        wchar_t value[128] = {};
        GetPrivateProfileStringW(section, key, L"", value, 128, ini.c_str());
        return std::wstring(value);
    };
    std::wstring line;
    auto add = [&](const std::wstring &part) { line += (line.empty() ? L"" : L"  \u00B7  ") + part; };
    std::wstring mpv = get(L"mpv", L"version");
    if (!mpv.empty()) add(L"mpv " + mpv.substr(0, mpv.find(L"-git")));
    if (const std::wstring r = get(L"reshade", L"version"); !r.empty()) add(L"ReShade " + r);
    std::wstring renodx = get(L"dlss5", L"renodx");
    if (!renodx.empty()) add(L"DLSS 5 " + renodx.substr(renodx.find_last_of(L'-') + 1));
    return narrow(line);
}

Choice from(const Settings &s) {
    Choice c;
    c.device = s.device;
    c.width = s.width;
    c.height = s.height;
    c.fps = s.fps;
    c.hdr = s.hdr;
    c.dlss5 = s.dlss5 != Settings::Dlss5::Off;
    c.framegen = s.framegen;
    c.fast = s.framegen_fast;
    c.audio = s.audio;
    c.audio_device = s.audio_device;
    c.borderless = s.borderless;
    c.show = s.show_launcher;
    return c;
}

std::string windows_font(const char *file) {
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    const std::string path = std::string(dir) + "\\Fonts\\" + file;
    return GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : path;
}

}  // namespace

LauncherAction run_launcher(const std::wstring &settings_path, const std::wstring &engine_dir,
                            const LauncherExtras &extras) {
    BugReport bug;  // joined last, after the window is gone
    Settings settings;
    std::wstring error;
    if (!load_settings(settings_path, settings, error)) log(L"launcher: {}", error);  // shown again on Play

    Window w;
    g_window = &w;
    if (!open_window(w)) {
        g_window = nullptr;
        return LauncherAction::Play;  // no window: play as before
    }
    Survey survey;
    survey.start();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
#ifdef NDEBUG
    io.ConfigDebugHighlightIdConflicts = false;  // a developer's warning, not for players
#endif
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const std::string font = windows_font("segoeui.ttf");
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str())) io.Fonts->AddFontDefault();
    const std::string bold = windows_font("seguisb.ttf");
    if (!bold.empty()) io.Fonts->AddFontFromFileTTF(bold.c_str());
    ImGui_ImplWin32_Init(w.hwnd);
    ImGui_ImplDX11_Init(w.device.Get(), w.context.Get());

    Screen screen{from(settings), from(settings)};
    screen.dlss5_installed = component_installed(engine_dir, Component::Dlss5);
    screen.dlss5_forced = settings.dlss5 == Settings::Dlss5::On;
    screen.versions = versions_line(engine_dir);
    screen.extras = &extras;
    screen.bug = &bug;
    float style_scale = 0;

    while (!screen.finished && !w.closed) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (w.closed) break;
        if (IsIconic(w.hwnd)) {
            Sleep(30);
            continue;
        }
        if (w.resize_w && w.resize_h) {
            w.rtv.Reset();
            w.swapchain->ResizeBuffers(0, w.resize_w, w.resize_h, DXGI_FORMAT_UNKNOWN, 0);
            w.resize_w = w.resize_h = 0;
            make_target(w);
        }
        if (style_scale != w.dpi_scale) {
            ui::apply_style(w.dpi_scale * 0.9f, 1.0f);
            style_scale = w.dpi_scale;
        }
        if (survey.done() && !screen.found) screen.found = &survey.found();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw(screen);
        ImGui::Render();
        // The window takes the height its contents need (up to the screen's).
        RECT client{};
        GetClientRect(w.hwnd, &client);
        const int want = int(std::ceil(screen.content_height));
        if (want > 0 && std::abs(want - int(client.bottom)) > 2) {
            RECT r{0, 0, client.right, want};
            const UINT dpi = GetDpiForWindow(w.hwnd);
            AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongPtrW(w.hwnd, GWL_STYLE)), FALSE, 0, dpi);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            RECT now{};
            GetWindowRect(w.hwnd, &now);
            const int height = std::min(int(r.bottom - r.top), int(mi.rcWork.bottom - mi.rcWork.top));
            const int top = std::max(mi.rcWork.top, std::min(now.top, mi.rcWork.bottom - height));
            SetWindowPos(w.hwnd, nullptr, now.left, top, r.right - r.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        ID3D11RenderTargetView *target = w.rtv.Get();
        w.context->OMSetRenderTargets(1, &target, nullptr);
        w.context->ClearRenderTargetView(target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        w.swapchain->Present(1, 0);
    }

    // Kept for Play, and before setup (the window comes back after it).
    if (screen.finished) save(settings_path, screen.c, screen.original, screen.found && is_rtx50(screen.found->gpu));
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    w.rtv.Reset();
    w.swapchain.Reset();
    w.context.Reset();
    w.device.Reset();
    DestroyWindow(w.hwnd);
    g_window = nullptr;
    return screen.finished ? screen.action : LauncherAction::Quit;
}

}  // namespace glasir
