# SPDX-License-Identifier: MIT
# Capture: finding cards, reading frames (Media Foundation), dedup.

add_library(glasir_capture STATIC
    audio_endpoint.cpp
    capture.cpp
    devices.cpp
    log.cpp)
glasir_component(glasir_capture)
target_include_directories(glasir_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(glasir_capture PUBLIC
    mfplat mf mfreadwrite mfuuid strmiids ole32 oleaut32 cfgmgr32)
