add_executable(build_grammar build_grammar.cc
  ../src/gram_db.cc
  ../src/gram_encoding.cc)
target_include_directories(build_grammar PRIVATE ../src)
target_link_libraries(build_grammar PRIVATE ${rime_library})
set_target_properties(build_grammar PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(build_grammar PRIVATE RIME_IMPORTS)
endif()
if(MINGW)
  set_property(TARGET build_grammar APPEND_STRING PROPERTY LINK_FLAGS " -municode")
endif()
install(TARGETS build_grammar DESTINATION ${BIN_INSTALL_DIR})
