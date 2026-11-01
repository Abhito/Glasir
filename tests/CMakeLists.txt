# SPDX-License-Identifier: MIT

# Checks the toolchain, the generated version header and that the vendored
# third-party headers compile with our settings.
add_executable(build_smoke build_smoke.cpp)
glasir_component(build_smoke)
target_link_libraries(build_smoke PRIVATE glasir_reshade_api glasir_mpv_client)
add_test(NAME build_smoke COMMAND build_smoke)
