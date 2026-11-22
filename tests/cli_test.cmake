file(MAKE_DIRECTORY "${WORK}")
file(REMOVE "${WORK}/named-zh-hans.dlm"
            "${WORK}/wanxiang-lts-zh-hans.dlm")
file(WRITE "${WORK}/fixed one.txt" "ab\t \r\nac\t99999\t \r\n")
file(WRITE "${WORK}/fixed two.txt" "ad\n")
file(WRITE "${WORK}/long.txt" "abcde\n")
file(WRITE "${WORK}/precise.txt" "abc 20\nabcdef 50\n")
execute_process(
  COMMAND "${BUILDER}" --section collocations -m "4字2+2区域用途联想"
          -t "fixed one.txt" -w 5000
          -t "fixed two.txt" -w 5000
          --section long_phrases -t long.txt -w 5000000000
          --section weighted -t precise.txt -m "逐词权重" --name named --language zh-hans
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "multi-file build failed: ${output} ${error}")
endif()
file(READ "${WORK}/named-zh-hans.dlm" format LIMIT 18 HEX)
if(NOT format STREQUAL "52696d653a3a45766f4772616d6d61722f32")
  message(FATAL_ERROR "unexpected model format: ${format}")
endif()
file(READ "${WORK}/named-zh-hans.dlm" count OFFSET 32 LIMIT 8 HEX)
if(NOT count STREQUAL "0300000000000000")
  message(FATAL_ERROR "adjacent equal weights were not merged: ${count}")
endif()
execute_process(
  COMMAND "${BUILDER}" --inspect "${WORK}/named-zh-hans.dlm"
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "model inspection failed: ${output} ${error}")
endif()
if(EXPECT_LOGGING AND
   (NOT "${output}${error}" MATCHES "name=\"collocations\" weight=5000 score=[0-9]+ description=\"4字2[+]2区域用途联想\"" OR
    NOT "${output}${error}" MATCHES "name=\"long_phrases\" weight=5000000000 score=[0-9]+" OR
    NOT "${output}${error}" MATCHES "name=\"weighted\" weight=per-key description=\"逐词权重\""))
  message(FATAL_ERROR "incomplete section metadata: ${output} ${error}")
endif()
execute_process(
  COMMAND "${BUILDER}" -t precise.txt
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR
   NOT EXISTS "${WORK}/wanxiang-lts-zh-hans.dlm")
  message(FATAL_ERROR "default output name/language failed")
endif()
execute_process(
  COMMAND "${BUILDER}" -t precise.txt -m "重新构建"
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "replacing an existing model failed")
endif()
file(GLOB temporary_directories "${WORK}/.grammar-*")
if(temporary_directories)
  message(FATAL_ERROR "temporary model directory left behind")
endif()
foreach(case IN ITEMS missing_weight missing_file invalid_frequency conflicting_descriptions removed_config)
  file(REMOVE "${WORK}/${case}-zh-hans.dlm")
  if(case STREQUAL "missing_weight")
    set(args -w 5000 -t precise.txt)
  elseif(case STREQUAL "missing_file")
    set(args -t nonexistent.txt)
  elseif(case STREQUAL "invalid_frequency")
    file(WRITE "${WORK}/bad.txt" "abc nonsense\n")
    set(args -t bad.txt -w 5000)
  elseif(case STREQUAL "conflicting_descriptions")
    set(args -m first -t precise.txt -m second)
  else()
    set(args --config nonexistent.yaml -t precise.txt)
  endif()
  execute_process(COMMAND "${BUILDER}" ${args} --name "${case}"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
  if(status EQUAL 0 OR EXISTS "${WORK}/${case}-zh-hans.dlm")
    message(FATAL_ERROR "invalid input accepted: ${case}")
  endif()
endforeach()
file(WRITE "${WORK}/collision-zh-hans.dlm" "abc 10\n")
file(SHA256 "${WORK}/collision-zh-hans.dlm" before)
execute_process(
  COMMAND "${BUILDER}" -t collision-zh-hans.dlm --name collision
  WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE status)
file(SHA256 "${WORK}/collision-zh-hans.dlm" after)
if(status EQUAL 0 OR NOT before STREQUAL after)
  message(FATAL_ERROR "output overwrote an input file")
endif()
