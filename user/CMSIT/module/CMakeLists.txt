include_directories(include)
aux_source_directory(src MODULE_SRC)

add_library(${EUDAQ_MODULE} SHARED ${MODULE_SRC})
target_link_libraries(${EUDAQ_MODULE} ${EUDAQ_CORE_LIBRARY} ${Boost_SERIALIZATION_LIBRARY})

install(TARGETS
  ${EUDAQ_MODULE}
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
