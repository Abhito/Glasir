// SPDX-License-Identifier: MIT
// NVIDIA Reflex's low latency mode for a Direct3D device, through the
// NVIDIA driver's own nvapi64.dll (System32): nothing of NVIDIA's is built
// into Glasir, and where there is no NVIDIA driver nothing is loaded.
//
// What it asks the driver for (NVAPI, "Reflex Low Latency"):
//   NvAPI_D3D_SetSleepMode    low latency mode on or off for the device, and
//                             boost (the GPU kept at full clock speed)
//   NvAPI_D3D_Sleep           once a frame, where the next frame's work
//                             starts: the driver waits there for as long as
//                             keeps frames from queueing before the GPU
//   NvAPI_D3D_SetLatencyMarker, NvAPI_D3D_GetLatency
//                             where a frame's stages are, and the driver's
//                             own timings of the last frames (for the log)
//   NvAPI_D3D_GetSleepStatus  whether the driver took the mode
//
// The calls are found by their numbers in nvapi64.dll (nvapi_QueryInterface),
// which is how NVIDIA's own nvapi64.lib finds them. The numbers and the
// structures are from NVIDIA's NVAPI SDK, R620 (github.com/NVIDIA/nvapi,
// commit 51e5aae4bc40, MIT; nvapi_interface.h and nvapi.h): the structures
// here are laid out as their first versions there (the only ones so far),
// which NVIDIA's header says drivers take since release 455. Their sizes
// and the places of their fields were checked against that nvapi.h with
// static_asserts; do it again when a field is added here.
//
// One thread at a time (the add-on calls it on mpv's render thread).
#pragma once
#include <windows.h>
#include <unknwn.h>

#include <cstdint>
#include <cstring>

namespace glasir {

class NvLowLatency {
public:
    // NV_LATENCY_MARKER_TYPE.
    enum Marker : uint32_t {
        SimulationStart = 0,
        SimulationEnd = 1,
        RenderSubmitStart = 2,
        RenderSubmitEnd = 3,
        PresentStart = 4,
        PresentEnd = 5,
    };

    // What the driver says of the device now.
    struct Status {
        bool low_latency = false;  // the mode is on in the driver (the control panel can overrule a request)
        bool sleep_seen = false;   // it sees the Sleep calls
        uint32_t slept_us = 0;     // how long the latest one waited
        // G-SYNC (or G-SYNC Compatible) is following this device's frames:
        // only told while its window is in front. And whether its presents
        // go straight to the screen (full screen or independent flip).
        bool vrr = false;
        bool direct_flip = false;
    };

    // The driver's timings over the frames it has finished lately (up to 64).
    struct Report {
        int frames = 0;
        double queue_ms = 0;   // waiting in Windows' queue for the GPU
        double gpu_ms = 0;     // the GPU working on the frame
        double total_ms = 0;   // from the frame's first marker to the GPU being done with it
    };

    // The driver's interface; false where there is none (no NVIDIA driver, or
    // one without these calls). Tried once.
    bool load() {
        if (tried_) return ready_;
        tried_ = true;
        // The driver's own, from System32 only.
        const HMODULE dll = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return false;
        using Query = void *(__cdecl *)(unsigned int);
        const auto query = reinterpret_cast<Query>(GetProcAddress(dll, "nvapi_QueryInterface"));
        if (!query) return false;
        const auto initialize = reinterpret_cast<int(__cdecl *)()>(query(0x0150E828));  // NvAPI_Initialize
        set_mode_ = reinterpret_cast<SetMode>(query(0xAC1CA9E0));                        // NvAPI_D3D_SetSleepMode
        sleep_ = reinterpret_cast<Sleep>(query(0x852CD1D2));                             // NvAPI_D3D_Sleep
        status_ = reinterpret_cast<GetStatus>(query(0xAEF96CA1));                        // NvAPI_D3D_GetSleepStatus
        marker_ = reinterpret_cast<SetMarker>(query(0xD9984C05));                        // NvAPI_D3D_SetLatencyMarker
        latency_ = reinterpret_cast<GetLatency>(query(0x1A587F9C));                      // NvAPI_D3D_GetLatency
        if (!initialize || !set_mode_ || !sleep_ || !status_) return false;
        ready_ = initialize() == 0;
        return ready_;
    }

    // Whether this device has the mode: an NVIDIA GPU's (the driver answers
    // for it). The status asked for is not used.
    bool offered(IUnknown *device) {
        Status s;
        return load() && status(device, s);
    }

    // Low latency mode on or off for the device, with or without boost.
    // NVAPI's status: 0 is success.
    int set_mode(IUnknown *device, bool on, bool boost) {
        if (!load()) return -1;
        SleepMode p{};
        p.version = version<SleepMode>();
        p.low_latency = on ? 1 : 0;
        p.boost = on && boost ? 1 : 0;
        return set_mode_(device, &p);
    }

    // Once a frame, after its present: the driver waits here if it has to.
    void sleep(IUnknown *device) {
        if (ready_) sleep_(device);
    }

    // A stage of frame `frame` (a number that grows with every frame) starts
    // or ends now.
    void mark(IUnknown *device, Marker type, uint64_t frame) {
        if (!ready_ || !marker_) return;
        MarkerParams p{};
        p.version = version<MarkerParams>();
        p.frame = frame;
        p.type = type;
        marker_(device, &p);
    }

    bool status(IUnknown *device, Status &out) {
        if (!ready_) return false;
        SleepStatus p{};
        p.version = version<SleepStatus>();
        if (status_(device, &p) != 0) return false;
        out.low_latency = p.low_latency != 0;
        out.sleep_seen = p.use_game_sleep != 0;
        out.slept_us = p.sleep_interval_us;
        out.vrr = p.fullscreen_vrr != 0;
        out.direct_flip = p.fullscreen_iflip != 0;
        return true;
    }

    // The driver's timings of the frames marked lately; false when it has
    // none (it wants about 90 marked frames first).
    bool report(IUnknown *device, Report &out) {
        if (!ready_ || !latency_) return false;
        std::memset(&result_, 0, sizeof(result_));
        result_.version = version<LatencyResult>();
        if (latency_(device, &result_) != 0) return false;
        out = Report{};
        double queue = 0, gpu = 0, total = 0;
        for (const FrameReport &r : result_.frames) {
            if (r.frame == 0 || r.gpu_render_end == 0 || r.sim_start == 0 || r.gpu_render_end < r.sim_start) continue;
            ++out.frames;
            queue += r.os_queue_end >= r.os_queue_start ? double(r.os_queue_end - r.os_queue_start) : 0;
            gpu += double(r.gpu_active_us);
            total += double(r.gpu_render_end - r.sim_start);
        }
        if (out.frames == 0) return false;
        out.queue_ms = queue / out.frames / 1000.0;  // the driver's times are in microseconds
        out.gpu_ms = gpu / out.frames / 1000.0;
        out.total_ms = total / out.frames / 1000.0;
        return true;
    }

private:
    // NV_SET_SLEEP_MODE_PARAMS_V1.
    struct SleepMode {
        uint32_t version;
        uint8_t low_latency;
        uint8_t boost;
        uint32_t minimum_interval_us;  // 0: no frame rate limit
        uint8_t use_markers_to_optimize;
        uint8_t use_min_queue_time;
        uint8_t reserved[30];
    };
    // NV_GET_SLEEP_STATUS_PARAMS_V1.
    struct SleepStatus {
        uint32_t version;
        uint8_t low_latency;
        uint8_t fullscreen_vrr;
        uint8_t control_panel_vsync;
        uint32_t sleep_interval_us;
        uint8_t use_game_sleep;
        uint8_t fullscreen_iflip;
        uint8_t fg_multiplier;
        uint8_t dfg_control;
        uint32_t dfg_frame_time_target_us;
        uint8_t reserved[114];
    };
    // NV_LATENCY_MARKER_PARAMS_V1.
    struct MarkerParams {
        uint32_t version;
        uint64_t frame;
        uint32_t type;
        uint64_t reserved0;
        uint8_t reserved[56];
    };
    // NV_LATENCY_RESULT_PARAMS_V1 and its FrameReport.
    struct FrameReport {
        uint64_t frame;
        uint64_t input_sample;
        uint64_t sim_start, sim_end;
        uint64_t render_submit_start, render_submit_end;
        uint64_t present_start, present_end;
        uint64_t driver_start, driver_end;
        uint64_t os_queue_start, os_queue_end;
        uint64_t gpu_render_start, gpu_render_end;
        uint32_t gpu_active_us;
        uint32_t gpu_frame_us;
        uint64_t camera_constructed;
        uint32_t cross_adapter_copy_us;
        uint32_t ai_frame_us;
        uint8_t reserved[104];
    };
    struct LatencyResult {
        uint32_t version;
        FrameReport frames[64];
        uint8_t reserved[32];
    };
    static_assert(sizeof(SleepMode) == 44 && sizeof(SleepStatus) == 136 && sizeof(MarkerParams) == 88 &&
                      sizeof(FrameReport) == 240 && sizeof(LatencyResult) == 15400,
                  "NVAPI's structures, as nvapi.h lays them out");

    // MAKE_NVAPI_VERSION(type, 1): the structure's size, and its version above it.
    template <typename T>
    static constexpr uint32_t version() {
        return uint32_t(sizeof(T)) | (1u << 16);
    }

    using SetMode = int(__cdecl *)(IUnknown *, SleepMode *);
    using Sleep = int(__cdecl *)(IUnknown *);
    using GetStatus = int(__cdecl *)(IUnknown *, SleepStatus *);
    using SetMarker = int(__cdecl *)(IUnknown *, MarkerParams *);
    using GetLatency = int(__cdecl *)(IUnknown *, LatencyResult *);

    bool tried_ = false, ready_ = false;
    SetMode set_mode_ = nullptr;
    Sleep sleep_ = nullptr;
    GetStatus status_ = nullptr;
    SetMarker marker_ = nullptr;
    GetLatency latency_ = nullptr;
    LatencyResult result_{};  // 15 KB: not on the stack of every caller
};

}  // namespace glasir
