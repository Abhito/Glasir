# SPDX-License-Identifier: MIT
# Shared build settings for Glasir components.

# glasir_git_describe(<out-var>): short commit description, or "unknown"
# when building from a source archive without git.
function(glasir_git_describe out_var)
    set(result "unknown")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE described
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE git_result)
        if(git_result EQUAL 0 AND described)
            set(result "${described}")
        endif()
    endif()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# glasir_component(<target>): warnings, Unicode, lean Windows headers.
function(glasir_component target)
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(${target} PRIVATE glasir_version)
endfunction()
