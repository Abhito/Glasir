# SPDX-License-Identifier: MIT
# The in-game menu (MIT), built into the frame generation add-on so it can
# draw after frame generation on every presented frame.

set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_shader_dir}")
set(_headers)
foreach(_pair IN ITEMS "vs_5_0|VSMain|g_composite_vs|composite_vs.h" "ps_5_0|PSMain|g_composite_ps|composite_ps.h")
    string(REPLACE "|" ";" _s "${_pair}")
    list(GET _s 0 _profile)
    list(GET _s 1 _entry)
    list(GET _s 2 _variable)
    list(GET _s 3 _header)
    add_custom_command(
        OUTPUT "${_shader_dir}/${_header}"
        COMMAND "${GLASIR_FXC}" /nologo /T ${_profile} /E ${_entry} /O3 /Vn ${_variable}
                /Fh "${_shader_dir}/${_header}" "${CMAKE_CURRENT_SOURCE_DIR}/composite.hlsl"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/composite.hlsl"
        COMMENT "fxc composite.hlsl ${_entry}"
        VERBATIM)
    list(APPEND _headers "${_shader_dir}/${_header}")
endforeach()
add_custom_target(glasir_overlay_shaders DEPENDS ${_headers})

add_library(glasir_overlay STATIC compositor.cpp menu.cpp mpv_client.cpp overlay.cpp)
glasir_component(glasir_overlay)
add_dependencies(glasir_overlay glasir_overlay_shaders)
target_include_directories(glasir_overlay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${_shader_dir}")
target_link_libraries(glasir_overlay PUBLIC glasir_reshade_api glasir_imgui glasir_common d3d11 dxgi)
