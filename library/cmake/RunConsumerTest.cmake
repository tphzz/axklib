if(NOT DEFINED AXK_BINARY_DIR OR NOT DEFINED AXK_CONSUMER_SOURCE_DIR OR NOT DEFINED AXK_TEST_ROOT)
  message(FATAL_ERROR "consumer test requires binary, source, and test-root paths")
endif()

file(REMOVE_RECURSE "${AXK_TEST_ROOT}")
file(MAKE_DIRECTORY "${AXK_TEST_ROOT}")

set(install_command "${CMAKE_COMMAND}" --install "${AXK_BINARY_DIR}" --prefix "${AXK_TEST_ROOT}/install")
if(DEFINED AXK_BUILD_CONFIG AND NOT AXK_BUILD_CONFIG STREQUAL "")
  list(APPEND install_command --config "${AXK_BUILD_CONFIG}")
endif()
execute_process(COMMAND ${install_command} RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "failed to install axklib for consumer test")
endif()

set(configure_command
  "${CMAKE_COMMAND}"
  -S "${AXK_CONSUMER_SOURCE_DIR}"
  -B "${AXK_TEST_ROOT}/build"
  "-DCMAKE_PREFIX_PATH=${AXK_TEST_ROOT}/install"
  "-DAXK_TEST_FIXTURE=${AXK_TEST_FIXTURE}"
)
if(DEFINED AXK_GENERATOR AND NOT AXK_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${AXK_GENERATOR}")
endif()
if(DEFINED AXK_BUILD_CONFIG AND NOT AXK_BUILD_CONFIG STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${AXK_BUILD_CONFIG}")
endif()
if(DEFINED AXK_TOOLCHAIN_FILE AND NOT AXK_TOOLCHAIN_FILE STREQUAL "")
  list(APPEND configure_command "-DCMAKE_TOOLCHAIN_FILE=${AXK_TOOLCHAIN_FILE}")
endif()
if(DEFINED AXK_VCPKG_INSTALLED_DIR AND NOT AXK_VCPKG_INSTALLED_DIR STREQUAL "")
  list(APPEND configure_command "-DVCPKG_INSTALLED_DIR=${AXK_VCPKG_INSTALLED_DIR}")
endif()
if(DEFINED AXK_VCPKG_TARGET_TRIPLET AND NOT AXK_VCPKG_TARGET_TRIPLET STREQUAL "")
  list(APPEND configure_command "-DVCPKG_TARGET_TRIPLET=${AXK_VCPKG_TARGET_TRIPLET}")
endif()
if(DEFINED AXK_VCPKG_OVERLAY_TRIPLETS AND NOT AXK_VCPKG_OVERLAY_TRIPLETS STREQUAL "")
  list(APPEND configure_command "-DVCPKG_OVERLAY_TRIPLETS=${AXK_VCPKG_OVERLAY_TRIPLETS}")
endif()
execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "failed to configure external axklib consumer")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${AXK_TEST_ROOT}/build"
  RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "failed to build external axklib consumer")
endif()

execute_process(
  COMMAND "${AXK_TEST_ROOT}/build/axk_consumer"
  RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "external axklib consumer returned ${run_result}")
endif()
