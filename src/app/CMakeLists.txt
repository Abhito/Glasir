# SPDX-License-Identifier: MIT
# Glasir.exe: settings, capture card -> mpv.

add_library(glasir_app_core STATIC
    mpv_launch.cpp
    reshade_config.cpp
    settings.cpp)
glasir_component(glasir_app_core)
target_include_directories(glasir_app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(glasir_app_core PUBLIC glasir_capture)

add_executable(glasir WIN32 main.cpp)
set_target_properties(glasir PROPERTIES OUTPUT_NAME Glasir)
glasir_component(glasir)
target_link_libraries(glasir PRIVATE glasir_app_core shell32 user32)

install(TARGETS glasir RUNTIME DESTINATION .)
install(FILES mpv_config/mpv.conf mpv_config/input.conf DESTINATION engine/portable_config)
