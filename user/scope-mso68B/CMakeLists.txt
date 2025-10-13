list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_CURRENT_LIST_DIR}/cmake)
get_filename_component(EUDAQ_USERNAME ${CMAKE_CURRENT_LIST_DIR} NAME)
set(EUDAQ_MODULE module_${EUDAQ_USERNAME})
option(USER_SCOPE_MSO68B_BUILD "build user/scope-mso68B" OFF)

if(NOT USER_SCOPE_MSO68B_BUILD)
    message(STATUS "user/scope-mso68B is NOT to be built (USER_SCOPE_MSO68B_BUILD=OFF)")
  return()
endif()

message(STATUS "user/scope-mso68B is to be built (USER_SCOPE_MSO68B_BUILD=ON)")

add_subdirectory(module)
