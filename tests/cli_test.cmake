file(MAKE_DIRECTORY "${WORK}")
file(REMOVE "${WORK}/named-zh-hans.gram"
            "${WORK}/wanxiang-lts-zh-hans.gram")
file(WRITE "${WORK}/fixed one.txt" "ab\t \r\nac\t99999\t \r\n")
file(WRITE "${WORK}/fixed two.txt" "ad\n")
file(WRITE "${WORK}/long.txt" "abcde\n")
file(WRITE "${WORK}/precise.txt" "abc 20\nabcdef 50\n")
execute_process(
  COMMAND "${BUILDER}" --section collocations -t "fixed one.txt" -w 5000
          -t "fixed two.txt" -w 5000
          --section long_phrases -t long.txt -w 5000000000
          --section weighted -t precise.txt --name named --language zh-hans
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "multi-file build failed: ${output} ${error}")
endif()
file(READ "${WORK}/named-zh-hans.gram" count OFFSET 32 LIMIT 8 HEX)
if(NOT count STREQUAL "0300000000000000")
  message(FATAL_ERROR "adjacent equal weights were not merged: ${count}")
endif()
execute_process(
  COMMAND "${BUILDER}" -t precise.txt
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR
   NOT EXISTS "${WORK}/wanxiang-lts-zh-hans.gram")
  message(FATAL_ERROR "default output name/language failed")
endif()
foreach(case IN ITEMS missing_weight missing_file invalid_frequency removed_config)
  file(REMOVE "${WORK}/${case}-zh-hans.gram")
  if(case STREQUAL "missing_weight")
    set(args -w 5000 -t precise.txt)
  elseif(case STREQUAL "missing_file")
    set(args -t nonexistent.txt)
  elseif(case STREQUAL "invalid_frequency")
    file(WRITE "${WORK}/bad.txt" "abc nonsense\n")
    set(args -t bad.txt -w 5000)
  else()
    set(args --config nonexistent.yaml -t precise.txt)
  endif()
  execute_process(COMMAND "${BUILDER}" ${args} --name "${case}"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
  if(status EQUAL 0 OR EXISTS "${WORK}/${case}-zh-hans.gram")
    message(FATAL_ERROR "invalid input accepted: ${case}")
  endif()
endforeach()
file(WRITE "${WORK}/collision-zh-hans.gram" "abc 10\n")
file(SHA256 "${WORK}/collision-zh-hans.gram" before)
execute_process(
  COMMAND "${BUILDER}" -t collision-zh-hans.gram --name collision
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
file(SHA256 "${WORK}/collision-zh-hans.gram" after)
if(status EQUAL 0 OR NOT before STREQUAL after)
  message(FATAL_ERROR "output overwrote an input file")
endif()
