project(rime-octagram)
cmake_minimum_required(VERSION 3.10)

option(BUILD_TOOLS "Build tools" ON)

aux_source_directory(src octagram_src)

add_library(rime-octagram-objs OBJECT ${octagram_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-octagram-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-octagram PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-octagram-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "octagram" PARENT_SCOPE)

if(BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(BUILD_TEST)
  add_executable(octagram_test tests/gram_db_test.cc src/gram_db.cc src/gram_encoding.cc src/octagram.cc)
  target_include_directories(octagram_test PRIVATE src)
  target_link_libraries(octagram_test PRIVATE ${rime_library} ${GTEST_LIBRARIES})
  if(BUILD_SHARED_LIBS)
    target_compile_definitions(octagram_test PRIVATE RIME_IMPORTS)
  endif()
  if(WIN32)
    set_target_properties(octagram_test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    if(MSVC)
      target_compile_options(octagram_test PRIVATE /utf-8)
    endif()
  endif()
  add_test(NAME octagram_test COMMAND octagram_test)
  add_executable(grammar_scope_test tests/grammar_scope_test.cc)
  target_link_libraries(grammar_scope_test PRIVATE ${rime_library} ${rime_gears_library} ${GTEST_LIBRARIES})
  if(BUILD_SHARED_LIBS)
    target_compile_definitions(grammar_scope_test PRIVATE RIME_IMPORTS)
  endif()
  if(WIN32)
    set_target_properties(grammar_scope_test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    if(MSVC)
      target_compile_options(grammar_scope_test PRIVATE /utf-8)
    endif()
  endif()
  add_test(NAME grammar_scope_test COMMAND grammar_scope_test)
  if(TARGET build_grammar)
    add_test(NAME octagram_cli
      COMMAND ${CMAKE_COMMAND} -DBUILDER=$<TARGET_FILE:build_grammar>
              -DWORK=${PROJECT_BINARY_DIR}/cli-test
              -DEXPECT_LOGGING=${ENABLE_LOGGING}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake)
  endif()
endif()
