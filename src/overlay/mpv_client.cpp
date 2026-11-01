// SPDX-License-Identifier: MIT
#include "mpv_client.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "json.h"

namespace glasir::overlay {
namespace {

// What the menu follows; the ids are the indexes.
const char *const kProperties[] = {
    "volume", "mute", "fullscreen", "user-data/glasir/audio-boost", "user-data/glasir/audio-delay", "width", "height",
    "user-data/glasir/scaling",
};

int to_int(const Json &v, int fallback) {
    if (v.is(Json::Type::Number)) return int(v.number());
    if (v.is(Json::Type::String)) return std::atoi(v.string().c_str());
    return fallback;
}

}  // namespace

std::string json_quote(const std::string &s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                out += hex;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

void MpvClient::start(const std::wstring &pipe) {
    stop();
    pipe_ = pipe;
    stopping_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
}

void MpvClient::stop() {
    if (thread_.joinable()) {
        {
            std::lock_guard lock(m_);
            stopping_ = true;
        }
        SetEvent(wake_);
        thread_.join();
    }
    if (wake_) CloseHandle(wake_);
    wake_ = nullptr;
}

MpvState MpvClient::state() const {
    std::lock_guard lock(m_);
    return state_;
}

void MpvClient::command(const std::vector<std::string> &words) {
    std::string line = "{\"command\":[";
    for (size_t i = 0; i < words.size(); ++i) line += (i ? "," : "") + json_quote(words[i]);
    line += "]}\n";
    {
        std::lock_guard lock(m_);
        if (queue_.size() > 64) queue_.erase(queue_.begin());  // mpv is gone or stuck: keep the latest
        queue_.push_back(std::move(line));
    }
    if (wake_) SetEvent(wake_);
}

Toast MpvClient::toast() const {
    std::lock_guard lock(m_);
    return toast_;
}

void MpvClient::on_line(const std::string &line) {
    if (line.find("\"client-message\"") != std::string::npos) {
        const auto j = Json::parse(line);
        const Json *args = j ? j->get("args") : nullptr;
        if (!args || args->items().size() < 2 || args->items()[0].string() != "glasir-toast") return;
        std::lock_guard lock(m_);
        toast_.text = args->items()[1].string();
        toast_.seconds = args->items().size() > 2 ? std::atoi(args->items()[2].string().c_str()) / 1000.0 : 0;
        if (toast_.seconds <= 0) toast_.seconds = 1.5;
        toast_.from_property = false;
        ++toast_.serial;
        return;
    }
    if (line.find("\"property-change\"") == std::string::npos) return;
    const auto j = Json::parse(line);
    if (!j) return;
    const Json *id = j->get("id");
    const Json *data = j->get("data");
    if (!id) return;
    const int n = int(id->number()) - 1;
    std::lock_guard lock(m_);
    auto say = [&](std::string text) {
        toast_.text = std::move(text);
        toast_.seconds = 1.5;
        toast_.from_property = true;
        ++toast_.serial;
    };
    switch (n) {
    case 0: {
        const double before = state_.volume;
        state_.volume = data ? data->number(-1) : -1;
        if (seen_volume_ && state_.volume >= 0 && std::lround(state_.volume) != std::lround(before))
            say("Volume: " + std::to_string(std::lround(state_.volume)) + "%");
        seen_volume_ = seen_volume_ || state_.volume >= 0;
        break;
    }
    case 1: {
        const bool before = state_.mute;
        state_.mute = data && data->boolean();
        if (seen_mute_ && state_.mute != before) say(state_.mute ? "Muted" : "Sound on");
        seen_mute_ = true;
        break;
    }
    case 2: state_.fullscreen = data && data->boolean(); break;
    case 3: state_.boost = data ? to_int(*data, -1) : -1; break;
    case 4: state_.delay = data ? to_int(*data, -1) : -1; break;
    case 5: state_.width = data ? to_int(*data, 0) : 0; break;
    case 6: state_.height = data ? to_int(*data, 0) : 0; break;
    case 7: state_.scaling = data && data->is(Json::Type::String) ? data->string() : ""; break;
    default: break;
    }
}

// Connects (again and again while mpv is not listening), follows the
// properties, sends what is queued. One overlapped handle, used only here.
void MpvClient::run() {
    OVERLAPPED read_ov{}, write_ov{};
    read_ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    char buffer[4096];
    std::string pending;
    for (;;) {
        {
            std::lock_guard lock(m_);
            if (stopping_) break;
        }
        HANDLE pipe = CreateFileW(pipe_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(wake_, 500);
            continue;
        }
        auto write = [&](const std::string &line) {
            DWORD written = 0;
            ResetEvent(write_ov.hEvent);
            if (!WriteFile(pipe, line.data(), DWORD(line.size()), nullptr, &write_ov) &&
                GetLastError() != ERROR_IO_PENDING)
                return false;
            return GetOverlappedResult(pipe, &write_ov, &written, TRUE) && written == line.size();
        };
        std::string observe;
        for (size_t i = 0; i < std::size(kProperties); ++i)
            observe += "{\"command\":[\"observe_property\"," + std::to_string(i + 1) + "," + json_quote(kProperties[i]) +
                       "]}\n";
        bool ok = write(observe);
        if (ok) {
            std::lock_guard lock(m_);
            state_.connected = true;
        }
        pending.clear();
        bool reading = false;
        while (ok) {
            if (!reading) {
                ResetEvent(read_ov.hEvent);
                if (!ReadFile(pipe, buffer, sizeof(buffer), nullptr, &read_ov) && GetLastError() != ERROR_IO_PENDING) {
                    ok = false;
                    break;
                }
                reading = true;
            }
            HANDLE events[] = {read_ov.hEvent, wake_};
            const DWORD w = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                DWORD got = 0;
                reading = false;
                if (!GetOverlappedResult(pipe, &read_ov, &got, FALSE) || !got) {
                    ok = false;
                    break;
                }
                pending.append(buffer, got);
                for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
                    on_line(pending.substr(0, nl));
                    pending.erase(0, nl + 1);
                }
            } else if (w == WAIT_OBJECT_0 + 1) {
                std::vector<std::string> lines;
                bool stopping = false;
                {
                    std::lock_guard lock(m_);
                    lines.swap(queue_);
                    stopping = stopping_;
                }
                if (stopping) break;
                for (const auto &line : lines) ok = ok && write(line);
            } else {
                ok = false;
            }
        }
        if (reading) {
            CancelIoEx(pipe, &read_ov);
            DWORD got = 0;
            GetOverlappedResult(pipe, &read_ov, &got, TRUE);
        }
        CloseHandle(pipe);
        {
            std::lock_guard lock(m_);
            state_ = MpvState{};
            seen_volume_ = seen_mute_ = false;
            if (stopping_) break;
        }
        WaitForSingleObject(wake_, 500);
    }
    CloseHandle(read_ov.hEvent);
    CloseHandle(write_ov.hEvent);
}

}  // namespace glasir::overlay
