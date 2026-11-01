# SPDX-License-Identifier: MIT
# Glasir.exe: settings, setup (component downloads), capture card -> mpv.

add_library(glasir_app_core STATIC
    components.cpp
    gpu.cpp
    http.cpp
    mpv_launch.cpp
    reshade_config.cpp
    settings.cpp)
glasir_component(glasir_app_core)
target_include_directories(glasir_app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(glasir_app_core PUBLIC glasir_capture glasir_common glasir_version dxgi winhttp bcrypt wintrust crypt32 version)

add_executable(glasir WIN32 main.cpp launcher.cpp setup_ui.cpp glasir.manifest)
set_target_properties(glasir PROPERTIES OUTPUT_NAME Glasir)
glasir_component(glasir)
target_link_libraries(glasir PRIVATE glasir_app_core glasir_imgui shell32 user32 comctl32 dwmapi d3d11)

install(TARGETS glasir RUNTIME DESTINATION .)
install(FILES mpv_config/mpv.conf mpv_config/input.conf DESTINATION engine/portable_config)
