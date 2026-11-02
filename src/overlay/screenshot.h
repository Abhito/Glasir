// SPDX-License-Identifier: MIT
// Glasir's screenshot (the s key): the picture as it reaches the screen,
// after scaling and DLSS 5, without the menu or toasts and without the black
// bars, in Glasir's Screenshots folder.
//
// The frame is taken at mpv's present, before frame generation draws the
// frame in between into the same back buffer, so it is always a real frame.
// The GPU copies the picture's part of the back buffer into a staging
// texture; it is read back on a later frame, once the copy is done (never
// waiting on the GPU), and a thread of its own converts and writes the files.
//
// SDR: one PNG. HDR (PQ or scRGB): a JPEG XR file (.jxr), the HDR format
// Windows' own screenshots use (half-float scRGB; Photos shows it in HDR),
// and a PNG tone-mapped to SDR for sharing.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "compositor.h"  // Encoding

namespace glasir::overlay::shot {

// A picture read back from the GPU: 4 bytes a pixel (8 for
// R16G16B16A16_FLOAT), rows tightly packed.
struct Frame {
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    Encoding encoding = Encoding::Sdr;
    std::vector<BYTE> pixels;
};

// Back buffer formats it can save.
bool supported(DXGI_FORMAT format);

// Whether this frame is HDR: PQ in 10 bits, or scRGB (half floats).
bool is_hdr(const Frame &frame);

// The reference (paper) white HDR is tone-mapped from: BT.2408's 203 nits,
// as the menu is drawn in HDR.
constexpr float kReferenceWhiteNits = 203.0f;

// Linear BT.709 light in scRGB units (1.0 = 80 nits) of one pixel of an
// HDR frame; values outside BT.709 stay as they are (negative, or above).
void scrgb(const Frame &frame, size_t pixel, float rgb[3]);

// SDR sRGB (0-255, B G R order) of HDR light in scRGB units: the colours
// outside BT.709 brought in toward grey, then everything above
// reference_white_nits * knee rolled off smoothly to the SDR white, on the
// brightest channel so that hues and saturation stay.
void tone_map(const float scrgb[3], float reference_white_nits, BYTE bgr[3]);

// Writes stem.png (and stem.jxr for HDR) into folder. files: the names
// written. error: why not, for the log.
bool save(const Frame &frame, const std::wstring &folder, const std::wstring &stem, std::vector<std::wstring> &files,
          std::string &error);

// Takes the frame on the render thread and hands it to a thread that saves
// it. One at a time.
class Grabber {
public:
    bool busy() const { return staging_ != nullptr; }

    // Copies box of back_buffer (the picture, without bars) for saving.
    // False if this format cannot be saved or the copy cannot be made.
    bool grab(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *back_buffer, Encoding encoding,
              const D3D11_BOX &box, std::string &error);

    // Every frame: once the GPU has made the copy, reads it back and starts
    // the thread that saves it; it calls done(ok, text) when the files are
    // written (text: the folder, or why not).
    void tick(ID3D11DeviceContext *context, void (*done)(bool ok, const std::string &text));

    void release() { staging_.Reset(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    Frame frame_;  // the size, format and encoding; pixels on readback
    std::wstring stem_;
    int tries_ = 0;
};

}  // namespace glasir::overlay::shot
