find_program(STRATO_DRIVER_PATCH NAMES patch REQUIRED)
set(driver_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/adrenotools/custom-driver-failure.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${driver_patch}")
set(driver_source "${CMAKE_CURRENT_LIST_DIR}/../libraries/adrenotools")
execute_process(COMMAND "${STRATO_DRIVER_PATCH}" -p1 --reverse --dry-run --force -i "${driver_patch}"
    WORKING_DIRECTORY "${driver_source}" RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(NOT already EQUAL 0)
    execute_process(COMMAND "${STRATO_DRIVER_PATCH}" -p1 --forward --batch -i "${driver_patch}"
        WORKING_DIRECTORY "${driver_source}" RESULT_VARIABLE applied)
    if(NOT applied EQUAL 0)
        message(FATAL_ERROR "Custom driver patch failed: initialize the pinned adrenotools submodule")
    endif()
endif()
