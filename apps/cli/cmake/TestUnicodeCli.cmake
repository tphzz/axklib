if(NOT DEFINED AXK_CLI OR NOT DEFINED AXK_TEST_ROOT)
  message(FATAL_ERROR "AXK_CLI and AXK_TEST_ROOT are required")
endif()

set(test_root "${AXK_TEST_ROOT}/unicode-cli-ä-日本")
set(manifest "${test_root}/manifest-音.json")
set(image "${test_root}/output-音.hds")
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
file(WRITE "${manifest}"
     "{\"schema_version\":\"1.0\",\"size_bytes\":1048576,"
     "\"partitions\":[{\"name\":\"hd1\",\"volumes\":[{\"name\":\"Volume\","
     "\"waveforms\":[],\"samples\":[]}]}]}")

execute_process(
  COMMAND "${AXK_CLI}" create hds "${manifest}" --output "${image}"
  RESULT_VARIABLE create_result
  OUTPUT_VARIABLE create_output
  ERROR_VARIABLE create_error
  ENCODING UTF-8
)
if(NOT create_result EQUAL 0 OR NOT EXISTS "${image}")
  message(FATAL_ERROR
          "Unicode CLI create failed (${create_result}): ${create_output}${create_error}")
endif()

execute_process(
  COMMAND "${AXK_CLI}" info --format json "${image}"
  RESULT_VARIABLE info_result
  OUTPUT_VARIABLE info_output
  ERROR_VARIABLE info_error
  ENCODING UTF-8
)
if(NOT info_result EQUAL 0)
  message(FATAL_ERROR
          "Unicode CLI reopen failed (${info_result}): ${info_output}${info_error}")
endif()
string(FIND "${info_output}" "output-音.hds" path_position)
if(path_position EQUAL -1)
  message(FATAL_ERROR "Unicode source path is missing from CLI JSON output: ${info_output}")
endif()

file(REMOVE_RECURSE "${test_root}")
