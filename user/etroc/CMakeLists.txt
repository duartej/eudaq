list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_CURRENT_LIST_DIR}/cmake)
get_filename_component(EUDAQ_USERNAME ${CMAKE_CURRENT_LIST_DIR} NAME)
set(EUDAQ_MODULE module_${EUDAQ_USERNAME})
option(USER_ETROC_BUILD "build user/etroc" OFF)

if(NOT USER_ETROC_BUILD)
    message(STATUS "user/etroc is NOT to be built (USER_ETROC_BUILD=OFF)")
    return()
else()
    message(STATUS "user/etroc is to be built (USER_ETROC_BUILD=ON)")
endif()

add_subdirectory(module)
