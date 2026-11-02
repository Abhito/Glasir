# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

# The product name and version live here and nowhere else; code gets them
# from the generated glasir_version.h.
project(Glasir
    VERSION 0.1.2
    DESCRIPTION "Modern PC graphics for game consoles through a capture card"
    HOMEPAGE_URL "https://github.com/Abhito/Glasir"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Glasir builds on Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Glasir builds for 64-bit Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the app folder must run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/GlasirCommon.cmake)

# Version header (name, version, git commit) for every component.
glasir_git_describe(GLASIR_GIT_DESCRIBE)
configure_file(cmake/glasir_version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/glasir_version.h" @ONLY)
add_library(glasir_version INTERFACE)
target_include_directories(glasir_version INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Vendored third-party headers (see third_party/README.md).
add_library(glasir_reshade_api INTERFACE)
target_include_directories(glasir_reshade_api SYSTEM INTERFACE third_party/reshade/include)
add_library(glasir_mpv_client INTERFACE)
target_include_directories(glasir_mpv_client SYSTEM INTERFACE third_party/mpv/include)

# fxc from the Windows SDK the build uses (shaders of the add-ons).
set(_sdk_bin "C:/Program Files (x86)/Windows Kits/10/bin")
file(GLOB _sdk_versions LIST_DIRECTORIES true "${_sdk_bin}/10.*")
list(SORT _sdk_versions ORDER DESCENDING)
set(_fxc_hints "${_sdk_bin}/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
foreach(_v IN LISTS _sdk_versions)
    list(APPEND _fxc_hints "${_v}/x64")
endforeach()
find_program(GLASIR_FXC fxc HINTS ${_fxc_hints} REQUIRED)

# Dear ImGui (third_party/imgui, MIT): the in-game menu and the launcher.
# Built as it comes (its own warning level).
add_library(glasir_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(glasir_imgui SYSTEM PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_definitions(glasir_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE)
target_link_libraries(glasir_imgui PUBLIC d3d11 d3dcompiler dwmapi)

# Code shared by Glasir.exe and the add-ons (src/common).
add_library(glasir_common STATIC src/common/ini_edit.cpp src/common/json.cpp)
glasir_component(glasir_common)
target_include_directories(glasir_common PUBLIC src/common)

# Components are added as they are ported (docs/PLAN.md, "Porting order").
foreach(component IN ITEMS capture app audio overlay framegen)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${component}/CMakeLists.txt")
        add_subdirectory(src/${component})
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# The portable app folder: `cmake --install` puts everything here.
install(FILES LICENSE THIRD-PARTY-NOTICES.md DESTINATION .)
install(DIRECTORY LICENSES DESTINATION .)
install(FILES docs/README-release.txt DESTINATION . RENAME README.txt)

# The release zip (`build.cmd package`): the same folder, our files only;
# Glasir downloads mpv, ReShade and DLSS 5 on the user's PC.
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "Glasir-${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/out")
include(CPack)
