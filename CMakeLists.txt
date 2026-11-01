# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

# The product name and version live here and nowhere else; code gets them
# from the generated glasir_version.h.
project(Glasir
    VERSION 0.1.0
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

# Components are added as they are ported (docs/PLAN.md, "Porting order").
foreach(component IN ITEMS capture app audio framegen)
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
