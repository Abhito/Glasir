// SPDX-License-Identifier: MIT
#include "audio_endpoint.h"

#include <initguid.h>  // defines the DEVPKEY and PKEY constants used here
// In this order: mmdeviceapi.h brings DEFINE_PROPERTYKEY.
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <wrl/client.h>

#include <cwctype>

#include "devices.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace glasir {
namespace {

// Built-in devices all share this "container": it says nothing.
constexpr GUID kLocalMachine = {0x00000000, 0x0000, 0x0000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

bool useful_container(const GUID &g) { return g != GUID{} && g != kLocalMachine; }

std::wstring trim(std::wstring s) {
    const wchar_t *junk = L" -_()[]";
    const size_t a = s.find_first_not_of(junk);
    if (a == std::wstring::npos) return {};
    const size_t b = s.find_last_not_of(junk);
    return s.substr(a, b - a + 1);
}

}  // namespace

std::vector<AudioInput> list_audio_inputs() {
    std::vector<AudioInput> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return out;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return out;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;
        AudioInput input;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            input.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) input.name = v.pwszVal;
            PropVariantClear(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_ContainerId, &v)) && v.vt == VT_CLSID && v.puuid)
                input.container = *v.puuid;
            PropVariantClear(&v);
        }
        out.push_back(std::move(input));
    }
    return out;
}

std::optional<GUID> video_container(const std::wstring &symbolic_link) {
    if (symbolic_link.empty()) return std::nullopt;
    wchar_t instance[MAX_DEVICE_ID_LEN] = {};
    ULONG size = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(symbolic_link.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS)
        return std::nullopt;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return std::nullopt;
    GUID container{};
    size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(&container), &size,
                                 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_GUID)
        return std::nullopt;
    return container;
}

std::wstring card_base_name(const std::wstring &video_name) {
    std::wstring base = video_name;
    // Drop a trailing "Video" word ("-Video", " Video", "(Video)").
    for (;;) {
        base = trim(base);
        if (base.size() >= 5 && CompareStringOrdinal(base.c_str() + base.size() - 5, 5, L"Video", 5, TRUE) == CSTR_EQUAL)
            base.resize(base.size() - 5);
        else
            break;
    }
    return base;
}

std::optional<AudioInput> find_card_audio(const std::vector<AudioInput> &inputs, const std::wstring &video_name,
                                          const std::optional<GUID> &container, const std::wstring &wanted) {
    if (!wanted.empty()) {
        for (const auto &a : inputs)
            if (names_match(a.name, wanted) || a.id == wanted) return a;
        for (const auto &a : inputs)
            if (name_contains(a.name, wanted)) return a;
        return std::nullopt;
    }
    if (container && useful_container(*container))
        for (const auto &a : inputs)
            if (a.container == *container) return a;
    // By name, only if the name says something ("USB" alone would match
    // every USB microphone).
    const std::wstring base = card_base_name(video_name);
    if (base.size() >= 6 && !names_match(base, L"USB Capture") && !names_match(base, L"USB3 Capture"))
        for (const auto &a : inputs)
            if (name_contains(a.name, base)) return a;
    return std::nullopt;
}

}  // namespace glasir
