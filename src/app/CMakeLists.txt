# SPDX-License-Identifier: MIT
# Glasir.exe: settings, setup (component downloads), capture card -> mpv.

add_library(glasir_app_core STATIC
    backdrop.cpp
    bug_report.cpp
    components.cpp
    files.cpp
    gpu.cpp
    http.cpp
    mpv_launch.cpp
    reshade_config.cpp
    settings.cpp
    update.cpp)
glasir_component(glasir_app_core)
target_include_directories(glasir_app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(glasir_app_core PUBLIC glasir_capture glasir_common glasir_version dxgi winhttp bcrypt wintrust crypt32 version user32 ole32)

configure_file(glasir.rc.in "${CMAKE_CURRENT_BINARY_DIR}/glasir.rc" @ONLY)
add_executable(glasir WIN32 main.cpp launcher.cpp setup_ui.cpp glasir.manifest "${CMAKE_CURRENT_BINARY_DIR}/glasir.rc")
set_target_properties(glasir PROPERTIES OUTPUT_NAME Glasir)
glasir_component(glasir)
target_link_libraries(glasir PRIVATE glasir_app_core glasir_imgui shell32 user32 comctl32 dwmapi d3d11)

install(TARGETS glasir RUNTIME DESTINATION .)
install(FILES mpv_config/mpv.conf mpv_config/input.conf DESTINATION engine/portable_config)
install(FILES mpv_config/scripts/glasir_scaling.lua mpv_config/scripts/glasir_window.lua
        DESTINATION engine/portable_config/scripts)
install(FILES mpv_config/shaders/glasir_fsr1.glsl DESTINATION engine/portable_config/shaders)
# Third-party mpv shaders, unmodified (third_party/README.md): the cnn and
# anime4k scaling modes.
set(glasir_shaders "${PROJECT_SOURCE_DIR}/third_party")
install(FILES
    "${glasir_shaders}/artcnn/ArtCNN_C4F16.glsl"
    "${glasir_shaders}/anime4k/Anime4K_Clamp_Highlights.glsl"
    "${glasir_shaders}/anime4k/Anime4K_Restore_CNN_M.glsl"
    "${glasir_shaders}/anime4k/Anime4K_Upscale_CNN_x2_M.glsl"
    "${glasir_shaders}/anime4k/Anime4K_AutoDownscalePre_x2.glsl"
    "${glasir_shaders}/anime4k/Anime4K_AutoDownscalePre_x4.glsl"
    "${glasir_shaders}/anime4k/Anime4K_Upscale_CNN_x2_S.glsl"
    DESTINATION engine/portable_config/shaders)
