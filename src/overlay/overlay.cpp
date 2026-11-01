// SPDX-License-Identifier: MIT
#include "overlay.h"

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "compositor.h"
#include "menu.h"
#include "mpv_client.h"

using Microsoft::WRL::ComPtr;

namespace glasir::overlay {
namespace {

constexpr UINT kOpenKey = VK_TAB;
constexpr float kPaperNits = 203.0f;  // BT.2408's reference white, for the menu in HDR

struct Overlay {
    Host host;
    ID3D11Device *device = nullptr;  // mpv's, not held: release() comes before it goes
    ComPtr<ID3D11DeviceContext> context;
    ImGuiContext *imgui = nullptr;
    bool backend = false;
    Compositor compositor;
    MpvClient mpv;
    bool failed = false;
    std::atomic<bool> open{false};
    // mpv's effect runtime at this ReShade frame, while the menu is open
    // (for draw_again, on the same thread before the next frame).
    reshade::api::effect_runtime *runtime = nullptr;
    Encoding encoding = Encoding::Sdr;
    std::atomic<reshade::api::color_space> color_space{reshade::api::color_space::unknown};
    LARGE_INTEGER last{};
    UINT style_height = 0;
    std::wstring settings_path;
    bool source_hdr = false;
    // Frames the menu was drawn on since it opened: mpv's presents, and the
    // real frames frame generation presents itself.
    unsigned on_mpv = 0, on_real = 0;
};

Overlay g;

double seconds_since(LARGE_INTEGER &last) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double dt = last.QuadPart ? double(now.QuadPart - last.QuadPart) / double(freq.QuadPart) : 1.0 / 60.0;
    last = now;
    return dt > 0 ? dt : 1.0 / 60.0;
}

std::string windows_font(const char *file) {
    char dir[MAX_PATH] = {};
    GetWindowsDirectoryA(dir, MAX_PATH);
    const std::string path = std::string(dir) + "\\Fonts\\" + file;
    return GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string() : path;
}

bool init(ID3D11Device *device) {
    g.device = device;
    device->GetImmediateContext(&g.context);
    if (!g.compositor.init(device)) return false;
    IMGUI_CHECKVERSION();
    g.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g.imgui);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written into mpv's folder
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Segoe UI, which every Windows has (not shipped with Glasir).
    const std::string font = windows_font("segoeui.ttf");
    if (font.empty() || !io.Fonts->AddFontFromFileTTF(font.c_str())) io.Fonts->AddFontDefault();
    const std::string bold = windows_font("seguisb.ttf");
    if (!bold.empty()) io.Fonts->AddFontFromFileTTF(bold.c_str());
    g.backend = ImGui_ImplDX11_Init(device, g.context.Get());
    if (!g.backend) return false;

    wchar_t path[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"GLASIR_SETTINGS", path, MAX_PATH) > 0) g.settings_path = path;
    wchar_t mode[8] = {};
    g.source_hdr = GetEnvironmentVariableW(L"GLASIR_VIDEO_MODE", mode, 8) > 0 && _wcsicmp(mode, L"hdr") == 0;
    const std::string &p = g.host.mpv_pipe;
    g.mpv.start(L"\\\\.\\pipe\\" + std::wstring(p.begin(), p.end()));
    reshade::log::message(reshade::log::level::info, "Glasir menu: ready");
    return true;
}

Encoding encoding_of(reshade::api::color_space cs) {
    switch (cs) {
    case reshade::api::color_space::hdr10_st2084: return Encoding::Pq;
    case reshade::api::color_space::extended_srgb_linear: return Encoding::ScRgb;
    default: return Encoding::Sdr;
    }
}

// Keys and the mouse, from ReShade's input (it sees them even while it keeps
// them from mpv).
void feed_input(reshade::api::effect_runtime *runtime, UINT width, UINT height, bool take_wheel) {
    ImGuiIO &io = ImGui::GetIO();
    static const std::pair<UINT, ImGuiKey> keys[] = {
        {VK_UP, ImGuiKey_UpArrow},     {VK_DOWN, ImGuiKey_DownArrow}, {VK_LEFT, ImGuiKey_LeftArrow},
        {VK_RIGHT, ImGuiKey_RightArrow}, {VK_RETURN, ImGuiKey_Enter}, {VK_SPACE, ImGuiKey_Space},
        {VK_HOME, ImGuiKey_Home},      {VK_END, ImGuiKey_End},
    };
    for (const auto &[vk, key] : keys) io.AddKeyEvent(key, runtime->is_key_down(vk));

    uint32_t x = 0, y = 0;
    int16_t wheel = 0;
    // In the window's client area (ReShade converts it, whatever its comment
    // says), scaled in case the back buffer is not the window's size.
    runtime->get_mouse_cursor_position(&x, &y, &wheel);
    RECT client{};
    const HWND hwnd = static_cast<HWND>(runtime->get_hwnd());
    if (hwnd && GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0)
        io.AddMousePosEvent(float(x) * width / client.right, float(y) * height / client.bottom);
    else
        io.AddMousePosEvent(float(x), float(y));
    for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b == 0 ? 0 : b == 2 ? 1 : 2, runtime->is_mouse_button_down(b));
    if (wheel && take_wheel) io.AddMouseWheelEvent(0, std::abs(wheel) >= WHEEL_DELTA ? float(wheel) / WHEEL_DELTA : float(wheel));
}

void set_open(bool open) {
    if (open == g.open) return;
    g.open = open;
    if (open) {
        reshade::log::message(reshade::log::level::info, "Glasir menu: open");
    } else {
        char text[128];
        snprintf(text, sizeof(text), "Glasir menu: closed; it was drawn on %u frames mpv presented, %u real frames",
                 g.on_mpv, g.on_real);
        reshade::log::message(reshade::log::level::info, text);
    }
    g.on_mpv = g.on_real = 0;
    // mpv hides the cursor when the mouse rests; the menu needs it.
    g.mpv.command({"set", "cursor-autohide", open ? "no" : "100"});  // Glasir starts mpv with 100
    if (open) g.last.QuadPart = 0;
}

// Builds the menu for now and draws it on back_buffer. wheel: take the mouse
// wheel (once per ReShade frame: it is the movement since ReShade's last).
void build_and_draw(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, bool wheel) {
    reshade::api::effect_runtime *runtime = g.runtime;
    D3D11_TEXTURE2D_DESC bb{};
    back_buffer->GetDesc(&bb);
    g.encoding = encoding_of(g.color_space.load());

    ImGui::SetCurrentContext(g.imgui);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(bb.Width), float(bb.Height));
    io.DeltaTime = float(seconds_since(g.last));
    const float scale = std::max(0.5f, bb.Height / 1080.0f);
    if (bb.Height != g.style_height) {
        apply_style(scale);
        g.style_height = bb.Height;
    }
    feed_input(runtime, bb.Width, bb.Height, wheel);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    MenuFrame frame;
    frame.runtime = runtime;
    frame.host = &g.host;
    frame.mpv = &g.mpv;
    frame.encoding = g.encoding;
    frame.settings_path = g.settings_path;
    frame.source_hdr = g.source_hdr;
    frame.scale = scale;
    const bool keep = draw_menu(frame);
    ImGui::Render();

    StateBackup backup(context);
    ID3D11RenderTargetView *target = g.compositor.begin_menu(context, bb.Width, bb.Height);
    static bool reported = false;
    if (target) {
        context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!g.compositor.composite(context, back_buffer, g.encoding, kPaperNits) && !reported) {
            reported = true;
            reshade::log::message(reshade::log::level::warning, "Glasir menu: could not draw on this back buffer");
        }
    } else if (!reported) {
        reported = true;
        reshade::log::message(reshade::log::level::warning, "Glasir menu: could not make its texture");
    }
    if (!keep) set_open(false);
}

}  // namespace

void set_host(const Host &host) { g.host = host; }

void note_color_space(reshade::api::color_space color_space) { g.color_space = color_space; }

bool menu_open() { return g.open; }

void on_reshade_present(reshade::api::effect_runtime *runtime) {
    g.runtime = nullptr;
    reshade::api::device *api_device = runtime->get_device();
    if (g.failed || api_device->get_api() != reshade::api::device_api::d3d11) return;
    auto *device = reinterpret_cast<ID3D11Device *>(api_device->get_native());
    auto *swapchain = reinterpret_cast<IDXGISwapChain *>(runtime->get_native());
    if (!device || !swapchain) return;
    if (g.device && device != g.device) return;  // another device's swap chain
    static bool seen = false;
    if (!seen) {
        seen = true;
        reshade::log::message(reshade::log::level::info, "Glasir menu: Tab opens it");
    }

    if (runtime->is_key_pressed(kOpenKey) || (g.open && runtime->is_key_pressed(VK_ESCAPE))) set_open(!g.open);
    if (!g.open) return;
    if (!g.device && !init(device)) {
        g.failed = true;
        reshade::log::message(reshade::log::level::error, "Glasir menu: could not start; it stays off");
        set_open(false);
        return;
    }
    runtime->block_input_next_frame();
    ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) return;
    g.runtime = runtime;
    build_and_draw(g.context.Get(), back_buffer.Get(), true);
    ++g.on_mpv;
}

void draw_again(ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer) {
    // Built again rather than drawn again: the menu then answers at the rate
    // frames reach the screen, not at mpv's.
    if (!g.open || !g.runtime || !g.imgui) return;
    build_and_draw(context, back_buffer, false);
    ++g.on_real;
}

void release() {
    g.mpv.stop();
    if (g.backend) {
        ImGui::SetCurrentContext(g.imgui);
        ImGui_ImplDX11_Shutdown();
        g.backend = false;
    }
    if (g.imgui) ImGui::DestroyContext(g.imgui);
    g.imgui = nullptr;
    g.compositor.release();
    g.context.Reset();
    g.device = nullptr;
    g.runtime = nullptr;
    g.open = false;
    g.style_height = 0;
}

}  // namespace glasir::overlay
