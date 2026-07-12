if(NOT DEFINED AXK_LIBRARY_BINARY_DIR OR NOT DEFINED AXK_CONSUMER_SOURCE_DIR OR
   NOT DEFINED AXK_TEST_ROOT OR NOT DEFINED AXK_TEST_FIXTURE)
  message(FATAL_ERROR "build-tree consumer test requires binary, source, fixture, and root paths")
endif()

file(REMOVE_RECURSE "${AXK_TEST_ROOT}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${AXK_CONSUMER_SOURCE_DIR}"
    -B "${AXK_TEST_ROOT}"
    "-Daxklib_DIR=${AXK_LIBRARY_BINARY_DIR}"
    "-DAXK_TEST_FIXTURE=${AXK_TEST_FIXTURE}"
    "-DCMAKE_BUILD_TYPE=${AXK_BUILD_CONFIG}"
  RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "failed to configure build-tree SDK consumer")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${AXK_TEST_ROOT}"
  RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "failed to build build-tree SDK consumer")
endif()
execute_process(
  COMMAND "${AXK_TEST_ROOT}/axk_consumer"
  RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "build-tree SDK consumer returned ${run_result}")
endif()
