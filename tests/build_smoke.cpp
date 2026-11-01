// SPDX-License-Identifier: MIT
// Build check: our compiler settings, the generated version header and the
// vendored ReShade and mpv headers all compile together.
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <reshade.hpp>
#include <mpv/client.h>

#include "glasir_version.h"

static_assert(GLASIR_VERSION_MAJOR >= 0, "version header missing");
static_assert(RESHADE_API_VERSION >= 1, "ReShade API header missing");
static_assert(MPV_CLIENT_API_VERSION >= MPV_MAKE_VERSION(2, 0), "mpv client header too old");

int main() {
    std::printf("%s %s (%s), ReShade API %d, mpv client API %lu.%lu\n",
                GLASIR_NAME, GLASIR_VERSION, GLASIR_GIT_DESCRIBE, RESHADE_API_VERSION,
                MPV_CLIENT_API_VERSION >> 16, MPV_CLIENT_API_VERSION & 0xffff);
    return std::strcmp(GLASIR_NAME, "Glasir") == 0 ? 0 : 1;
}
