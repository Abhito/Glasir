# SPDX-License-Identifier: MIT

# Checks the toolchain, the generated version header and that the vendored
# third-party headers compile with our settings.
add_executable(build_smoke build_smoke.cpp)
glasir_component(build_smoke)
target_link_libraries(build_smoke PRIVATE glasir_reshade_api glasir_mpv_client)
add_test(NAME build_smoke COMMAND build_smoke)

add_executable(test_dedup test_dedup.cpp)
glasir_component(test_dedup)
target_link_libraries(test_dedup PRIVATE glasir_capture)
add_test(NAME dedup COMMAND test_dedup)

add_executable(test_launch test_launch.cpp)
glasir_component(test_launch)
target_link_libraries(test_launch PRIVATE glasir_app_core shell32)
add_test(NAME launch COMMAND test_launch)

add_executable(test_audio test_audio.cpp)
glasir_component(test_audio)
target_link_libraries(test_audio PRIVATE glasir_capture)
add_test(NAME audio COMMAND test_audio)

# VfgQuality=auto's choice of model: arithmetic only (src/framegen/fg_vfg_auto.h).
add_executable(test_vfg_auto test_vfg_auto.cpp)
glasir_component(test_vfg_auto)
target_include_directories(test_vfg_auto PRIVATE "${PROJECT_SOURCE_DIR}/src/framegen")
add_test(NAME vfg_auto COMMAND test_vfg_auto)
