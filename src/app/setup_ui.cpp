// SPDX-License-Identifier: MIT
#include "setup_ui.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <format>
#include <thread>

#include "log.h"

namespace glasir {
namespace {

constexpr int kDownload = 100;

HRESULT CALLBACK links(HWND, UINT note, WPARAM, LPARAM lparam, LONG_PTR) {
    if (note == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t *>(lparam), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

std::wstring megabytes(uint64_t bytes) { return std::format(L"{:.0f} MB", bytes / 1048576.0); }

struct Run {
    ProgressJob job;
    SetupProgress progress;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    bool ok = false;
    std::wstring error;
    std::thread worker;
    std::wstring shown_step;
    bool marquee = false;
};

void work(Run &run) {
    run.ok = run.job(run.progress, run.cancel, run.error);
    run.done = true;
}

HRESULT CALLBACK progress_events(HWND hwnd, UINT note, WPARAM wparam, LPARAM, LONG_PTR data) {
    Run &run = *reinterpret_cast<Run *>(data);
    switch (note) {
    case TDN_CREATED:
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
        run.worker = std::thread(work, std::ref(run));
        break;
    case TDN_TIMER: {
        if (run.done) {
            SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
            break;
        }
        if (run.cancel) break;
        const SetupProgress::State s = run.progress.get();
        if (s.step != run.shown_step) {
            run.shown_step = s.step;
            SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, reinterpret_cast<LPARAM>(s.step.c_str()));
        }
        std::wstring text = s.count > 1 ? std::format(L"Part {} of {}", s.index, s.count) : L"";
        if (s.done) text += (text.empty() ? L"" : L": ") + megabytes(s.done) + (s.total ? L" of " + megabytes(s.total) : L"");
        SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(text.c_str()));
        const bool marquee = s.total == 0;
        if (marquee != run.marquee) {
            run.marquee = marquee;
            SendMessageW(hwnd, TDM_SET_MARQUEE_PROGRESS_BAR, marquee, 0);
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, marquee, 30);
        }
        if (!marquee)
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, WPARAM(std::min<uint64_t>(1000, s.done * 1000 / s.total)), 0);
        break;
    }
    case TDN_BUTTON_CLICKED:
        if (wparam == IDCANCEL && !run.done) {
            // Ends at the next piece of the download; the window closes then.
            run.cancel = true;
            SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, reinterpret_cast<LPARAM>(L"Cancelling"));
            SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
            return S_FALSE;
        }
        break;
    }
    return S_OK;
}

}  // namespace

std::optional<std::vector<Component>> ask_setup(const SetupChoice &choice) {
    const std::wstring notices = L"<a href=\"" + choice.notices_file + L"\">THIRD-PARTY-NOTICES.md</a>";
    const std::wstring content =
        L"Glasir runs on programs made by others. They are downloaded from where their authors publish them, "
        L"and each keeps its own licence (" + notices + L"):\n\n"
        L"\x2022 mpv, the video player: github.com/zhongfly/mpv-winbuild (about 35 MB)\n"
        L"\x2022 ReShade with add-on support, for frame generation: reshade.me (about 5 MB)";
    const std::wstring footer =
        L"DLSS 5 is DLSS5-Feeder, RenoDX-DLSS5 and NVIDIA's DLSS neural rendering (from RHI's mirror, "
        L"github.com/RankFTW/rhi-repo), NVIDIA DLSS (github.com/NVIDIA/DLSS) and LumeniteFX.";
    const TASKDIALOG_BUTTON buttons[] = {{kDownload, L"Download"}};

    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT |
                (choice.dlss5_checked ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    c.pszWindowTitle = L"Glasir setup";
    c.pszMainIcon = TD_INFORMATION_ICON;
    c.pszMainInstruction = choice.first_run ? L"Glasir needs to download a few programs" : L"Download Glasir's components";
    c.pszContent = content.c_str();
    c.pButtons = buttons;
    c.cButtons = 1;
    c.nDefaultButton = kDownload;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    if (choice.offer_dlss5) {
        c.pszVerificationText = L"Also download DLSS 5 (NVIDIA RTX 30, 40 or 50 series, about 250 MB)";
        c.pszFooter = footer.c_str();
    }
    c.pfCallback = links;

    int button = 0;
    BOOL dlss5 = FALSE;
    if (FAILED(TaskDialogIndirect(&c, &button, nullptr, &dlss5)) || button != kDownload) return std::nullopt;
    std::vector<Component> which = {Component::Mpv, Component::ReShade};
    if (choice.offer_dlss5 && dlss5) which.push_back(Component::Dlss5);
    return which;
}

bool run_with_progress(const wchar_t *title, const ProgressJob &job, bool dialogs, std::wstring &error) {
    Run run;
    run.job = job;
    if (!dialogs) {
        work(run);
    } else {
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
        c.pszWindowTitle = title;
        c.pszMainInstruction = L"Starting";
        // Room for "Part 3 of 3: 120 MB of 250 MB" without the window resizing.
        c.pszContent = L"                                                                        ";
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pfCallback = progress_events;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&run);
        c.cxWidth = 260;
        const HRESULT hr = TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
        if (run.worker.joinable()) {
            run.worker.join();
        } else if (FAILED(hr)) {
            log(L"no progress window ({})", hresult_text(hr));
            work(run);
        }
    }
    error = run.error;
    return run.ok;
}

bool run_setup(const std::wstring &engine_dir, const std::vector<Component> &which, bool dialogs,
               std::wstring &error) {
    std::wstring names;
    for (Component c : which) names += (names.empty() ? L"" : L", ") + std::wstring(component_name(c));
    log(L"setup: downloading {}", names);
    const bool ok = run_with_progress(
        L"Glasir setup",
        [&](SetupProgress &progress, const std::atomic<bool> &cancel, std::wstring &e) {
            return install_components(engine_dir, which, progress, cancel, e);
        },
        dialogs, error);
    if (ok)
        log(L"setup: done");
    else
        log(L"setup: stopped: {}", error);
    return ok;
}

}  // namespace glasir
