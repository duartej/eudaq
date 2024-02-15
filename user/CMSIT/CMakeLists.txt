list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_CURRENT_LIST_DIR}/cmake)
get_filename_component(EUDAQ_USERNAME ${CMAKE_CURRENT_LIST_DIR} NAME)
set(EUDAQ_MODULE module_${EUDAQ_USERNAME})
option(USER_CMSIT_BUILD "build user/CMSIT" OFF)

if(NOT USER_CMSIT_BUILD)
  message(STATUS "user/CMSIT is NOT to be built (USER_CMSIT_BUILD=OFF)")
  return()
endif()
  
find_package(Boost 1.53 REQUIRED COMPONENTS serialization)
cmake_dependent_option(USER_CMSIT_BUILD "build user/CMSIT Converter Plugin" ON 
    "Boost_FOUND" OFF)
message(STATUS "user/CMSIT is to be built (USER_CMSIT_BUILD=ON)")

add_subdirectory(module)
