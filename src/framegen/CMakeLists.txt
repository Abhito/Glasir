# SPDX-License-Identifier: MPL-2.0
# Frame generation: the ReShade add-on (zz-glasir-framegen.addon64; "zz-" so
# ReShade loads it after the DLSS 5 add-ons) and its offline evaluator.

include(shaders.cmake)

# fxc (GLASIR_FXC) is found by the root CMakeLists.txt.

set(_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${_shader_dir}")
set(_shader_headers)
foreach(_item IN LISTS GLASIR_FRAMEGEN_SHADERS)
    string(REPLACE "|" ";" _shader "${_item}")
    list(GET _shader 0 _profile)
    list(GET _shader 1 _entry)
    list(GET _shader 2 _define)
    list(GET _shader 3 _variable)
    list(GET _shader 4 _header)
    list(GET _shader 5 _source)
    set(_define_arg)
    if(NOT _define STREQUAL "-")
        set(_define_arg /D ${_define})
    endif()
    add_custom_command(
        OUTPUT "${_shader_dir}/${_header}"
        COMMAND "${GLASIR_FXC}" /nologo /T ${_profile} /E ${_entry} ${_define_arg} /O3 /Vn ${_variable}
                /Fh "${_shader_dir}/${_header}" "${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
        COMMENT "fxc ${_source} ${_entry}"
        VERBATIM)
    list(APPEND _shader_headers "${_shader_dir}/${_header}")
endforeach()
add_custom_target(glasir_framegen_shaders DEPENDS ${_shader_headers})

# What the add-on and the evaluator share: fg_interp.h and the shaders.
add_library(glasir_framegen_interp INTERFACE)
target_include_directories(glasir_framegen_interp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" "${_shader_dir}")
add_dependencies(glasir_framegen_interp glasir_framegen_shaders)

add_library(glasir_framegen SHARED glasir_framegen.cpp)
set_target_properties(glasir_framegen PROPERTIES OUTPUT_NAME "zz-glasir-framegen" SUFFIX ".addon64")
glasir_component(glasir_framegen)
target_include_directories(glasir_framegen PRIVATE "${PROJECT_SOURCE_DIR}/src/common")
target_link_libraries(glasir_framegen PRIVATE glasir_framegen_interp glasir_reshade_api glasir_overlay
    d3d11 dxgi user32 ole32 windowscodecs)
install(TARGETS glasir_framegen RUNTIME DESTINATION engine)

# Offline evaluator (scores generated frames against true ones) and the
# steady-rate frame feeder for pipeline tests. Developer tools, not shipped.
add_executable(fg_eval eval/fg_eval.cpp)
glasir_component(fg_eval)
target_link_libraries(fg_eval PRIVATE glasir_framegen_interp d3d11 windowscodecs ole32)
target_compile_definitions(fg_eval PRIVATE _CRT_SECURE_NO_WARNINGS)  # getenv for its test switches

add_executable(raw_pace eval/raw_pace.cpp)
glasir_component(raw_pace)
