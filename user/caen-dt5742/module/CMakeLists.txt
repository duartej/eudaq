include_directories(include)
aux_source_directory(src MODULE_SRC)


# -----------------------------------------------------------------------------
# CAEN x742 offline correction / decode helper sources shipped with CAENDigitizer
# -----------------------------------------------------------------------------
set(CAEN_DIGITIZER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../extern/CAENDigitizer-v2.17.3")
set(CAEN_X742_SAMPLE_DIR "${CAEN_CURRENT_LIST_DIR}/../misc/x742_DataCorrection")
set(CAEN_DIGITIZER_INCLUDE_DIR "${CAEN_DIGITIZER_ROOT}/include")

find_library(CAENDIGITIZER_LIBRARY
  NAMES CAENDigitizer libCAENDigitizer
  PATHS /usr/lib /usr/lib64 /usr/local/lib
  REQUIRED
)
find_library(CAENCOMM_LIBRARY
  NAMES CAENComm libCAENComm
  PATHS /usr/lib /usr/lib64 /usr/local/lib
  REQUIRED
)
find_library(CAENVME_LIBRARY
  NAMES CAENVME CAENVMELib libCAENVME libCAENVMELib
  PATHS /usr/lib /usr/lib64 /usr/local/lib
  REQUIRED
)

add_library(caen_x742_routines STATIC
  "${CAEN_X742_SAMPLE_DIR}/X742CorrectionRoutines.c"
  "${CAEN_X742_SAMPLE_DIR}/X742DecodeRoutines.c"
)

set_target_properties(caen_x742_routines PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

set_source_files_properties(
  "${CAEN_X742_SAMPLE_DIR}/X742CorrectionRoutines.c"
  "${CAEN_X742_SAMPLE_DIR}/X742DecodeRoutines.c"
  PROPERTIES LANGUAGE C
)

target_include_directories(caen_x742_routines PUBLIC
  include
  "${CAEN_DIGITIZER_INCLUDE_DIR}"
  "${CAEN_X742_SAMPLE_DIR}"
)

target_link_libraries(caen_x742_routines PUBLIC
  ${CAENDIGITIZER_LIBRARY}
  ${CAENCOMM_LIBRARY}
  ${CAENVME_LIBRARY}
)

# -----------------------------------------------------------------------------

add_library(${EUDAQ_MODULE} SHARED ${MODULE_SRC})

# The correction tables for decoding
target_compile_definitions(${EUDAQ_MODULE} PRIVATE
  CAEN_DT5742_CORRECTION_TABLES_DIR="${CMAKE_CURRENT_LIST_DIR}/../misc/CorrectionTables"
)

target_include_directories(${EUDAQ_MODULE} PRIVATE
    include
    "${CAEN_DIGITIZER_INCLUDE_DIR}"
    "${CAEN_X742_SAMPLE_DIR}"
)

target_link_libraries(${EUDAQ_MODULE} 
    ${EUDAQ_CORE_LIBRARY} 
    ${EUDAQ_TTREE_LIBRARY} 
    ${EUDAQ_ROOT_MONITOR_LIBRARY}
    caen_x742_routines
    ${CAENDIGITIZER_LIBRARY}
    ${CAENCOMM_LIBRARY}
    ${CAENVME_LIBRARY}
)

set(EXE_CLI_ROOTCONVERTERWF caenCliRootWF)
add_executable(${EXE_CLI_ROOTCONVERTERWF} exe/caenCliRootWF.cxx)
target_link_libraries(${EXE_CLI_ROOTCONVERTERWF} ${EUDAQ_CORE_LIBRARY} ${EUDAQ_THREADS_LIB})
list(APPEND INSTALL_TARGETS ${EXE_CLI_ROOTCONVERTERWF})


install(TARGETS ${INSTALL_TARGETS}
  ${EUDAQ_MODULE}
  caen_x742_routines
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
