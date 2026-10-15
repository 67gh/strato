# Keep shader fixes in the parent repository: recursive submodule updates erase
# uncommitted changes in libraries/shader-compiler before CI configures CMake.
find_package(Git REQUIRED)
if(NOT DEFINED STRATO_SHADER_COMPILER_DIR)
    get_filename_component(STRATO_SHADER_COMPILER_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../libraries/shader-compiler" ABSOLUTE)
endif()
set(_strato_patch_dir "${CMAKE_CURRENT_LIST_DIR}/../patches/shader-compiler")
set(_strato_shader_files
    shader_info.h
    backend/spirv/spirv_emit_context.h
    backend/spirv/spirv_emit_context.cpp
    backend/spirv/emit_spirv_image.cpp
    backend/spirv/emit_spirv_image_atomic.cpp)
set(_strato_patch_files
    01-shader_info.h.patch
    02-spirv_emit_context.h.patch
    03-spirv_emit_context.cpp.patch
    04-emit_spirv_image.cpp.patch
    05-emit_spirv_image_atomic.cpp.patch)

# Prevent git from discovering a parent repository when building an exported
# source tree. Also ignore inherited git worktree overrides. Real submodules
# remain supported because their .git entry is at the working directory itself.
get_filename_component(_strato_shader_parent "${STRATO_SHADER_COMPILER_DIR}" DIRECTORY)
set(_strato_git "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
    "GIT_CEILING_DIRECTORIES=${_strato_shader_parent}"
    "${GIT_EXECUTABLE}" -C "${STRATO_SHADER_COMPILER_DIR}")

# Check the whole set before changing any source; never reset or overwrite a
# user's conflicting edits. A file already patched by the previous ZIP is OK.
set(_strato_pending)
foreach(_strato_patch IN LISTS _strato_patch_files)
    set(_strato_patch_path "${_strato_patch_dir}/${_strato_patch}")
    if(NOT EXISTS "${_strato_patch_path}")
        message(FATAL_ERROR "Missing shader compiler patch: ${_strato_patch_path}")
    endif()
    execute_process(COMMAND ${_strato_git} apply --reverse --check "${_strato_patch_path}"
        RESULT_VARIABLE _strato_reverse OUTPUT_QUIET ERROR_QUIET)
    if(NOT _strato_reverse EQUAL 0)
        execute_process(COMMAND ${_strato_git} apply --check "${_strato_patch_path}"
            RESULT_VARIABLE _strato_check OUTPUT_VARIABLE _strato_output ERROR_VARIABLE _strato_error)
        if(NOT _strato_check EQUAL 0)
            # Upgrade an already-patched checkout without resetting local work.
            set(_strato_upgrade "${_strato_patch_dir}/upgrades/${_strato_patch}")
            if(EXISTS "${_strato_upgrade}")
                execute_process(COMMAND ${_strato_git} apply --check "${_strato_upgrade}"
                    RESULT_VARIABLE _strato_check OUTPUT_VARIABLE _strato_output ERROR_VARIABLE _strato_error)
                if(_strato_check EQUAL 0)
                    set(_strato_patch_path "${_strato_upgrade}")
                endif()
            endif()
        endif()
        if(NOT _strato_check EQUAL 0)
            message(FATAL_ERROR
                "Cannot apply shader compiler patch ${_strato_patch}. "
                "Initialize the pinned submodule or resolve conflicting local edits. "
                "No patches from this invocation have been applied.\n${_strato_output}${_strato_error}")
        endif()
        list(APPEND _strato_pending "${_strato_patch_path}")
    endif()
endforeach()
foreach(_strato_patch_path IN LISTS _strato_pending)
    execute_process(COMMAND ${_strato_git} apply "${_strato_patch_path}"
        RESULT_VARIABLE _strato_apply OUTPUT_VARIABLE _strato_output ERROR_VARIABLE _strato_error)
    if(NOT _strato_apply EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${_strato_patch_path}:\n${_strato_output}${_strato_error}")
    endif()
    message(STATUS "Applied shader compiler patch: ${_strato_patch_path}")
endforeach()

# A submodule reset or a patch update must trigger reconfiguration with Ninja.
# Script mode is used by the host regression test and has no directory target.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    foreach(_strato_patch IN LISTS _strato_patch_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_strato_patch_dir}/${_strato_patch}")
        if(EXISTS "${_strato_patch_dir}/upgrades/${_strato_patch}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${_strato_patch_dir}/upgrades/${_strato_patch}")
        endif()
    endforeach()
    foreach(_strato_source IN LISTS _strato_shader_files)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${STRATO_SHADER_COMPILER_DIR}/${_strato_source}")
    endforeach()
endif()
