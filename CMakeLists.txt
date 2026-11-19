project(rime-octagram)
cmake_minimum_required(VERSION 3.10)

option(BUILD_TOOLS "Build tools" ON)

aux_source_directory(src octagram_src)

add_library(rime-octagram-objs OBJECT ${octagram_src})
target_include_directories(rime-octagram-objs PRIVATE ${Marisa_INCLUDE_PATH})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-octagram-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-octagram PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-octagram-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} ${Marisa_LIBRARY} PARENT_SCOPE)
set(plugin_modules "octagram" PARENT_SCOPE)

if(BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(BUILD_TEST)
  add_executable(octagram_db_test tests/gram_db_test.cc src/gram_db.cc src/gram_encoding.cc)
  target_include_directories(octagram_db_test PRIVATE src ${Marisa_INCLUDE_PATH})
  target_link_libraries(octagram_db_test PRIVATE ${rime_library} ${Marisa_LIBRARY} ${GTEST_LIBRARIES})
  if(BUILD_SHARED_LIBS)
    target_compile_definitions(octagram_db_test PRIVATE RIME_IMPORTS)
  endif()
  add_test(NAME octagram_db_test COMMAND octagram_db_test)
endif()
