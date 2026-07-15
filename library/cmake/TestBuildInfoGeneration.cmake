cmake_minimum_required(VERSION 3.28)

foreach(required_variable IN ITEMS AXK_BUILD_INFO_GENERATOR AXK_BUILD_INFO_MODULE
                                   AXK_BUILD_INFO_TEMPLATE AXK_PROJECT_VERSION_MODULE
                                   AXK_TEST_ROOT)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

find_package(Git REQUIRED)

function(run_git repository)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" ${ARGN}
    WORKING_DIRECTORY "${repository}"
    RESULT_VARIABLE result
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} failed: ${error}")
  endif()
endfunction()

function(generate repository output_cpp output_package output_metadata)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DAXK_VERSION_SOURCE_DIR=${repository}"
      -DAXK_VERSION_PRODUCT_NAME=axklib
      "-DAXK_VERSION_MODULE=${AXK_BUILD_INFO_MODULE}"
      "-DAXK_VERSION_PROJECT_MODULE=${AXK_PROJECT_VERSION_MODULE}"
      "-DAXK_VERSION_TEMPLATE=${AXK_BUILD_INFO_TEMPLATE}"
      "-DAXK_VERSION_OUTPUT_CPP=${output_cpp}"
      "-DAXK_VERSION_OUTPUT_PACKAGE=${output_package}"
      "-DAXK_VERSION_OUTPUT_METADATA=${output_metadata}"
      -DAXK_VERSION_EXPECTED_SEMANTIC=0.0.0
      -DAXK_VERSION_EXPECTED_PROJECT=0.0.0
      "-DAXK_VERSION_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${AXK_BUILD_INFO_GENERATOR}"
    RESULT_VARIABLE result
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "build-info generation failed")
  endif()
endfunction()

function(expect_generation_failure repository output_cpp output_package output_metadata expected_error)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DAXK_VERSION_SOURCE_DIR=${repository}"
      -DAXK_VERSION_PRODUCT_NAME=axklib
      "-DAXK_VERSION_MODULE=${AXK_BUILD_INFO_MODULE}"
      "-DAXK_VERSION_PROJECT_MODULE=${AXK_PROJECT_VERSION_MODULE}"
      "-DAXK_VERSION_TEMPLATE=${AXK_BUILD_INFO_TEMPLATE}"
      "-DAXK_VERSION_OUTPUT_CPP=${output_cpp}"
      "-DAXK_VERSION_OUTPUT_PACKAGE=${output_package}"
      "-DAXK_VERSION_OUTPUT_METADATA=${output_metadata}"
      -DAXK_VERSION_EXPECTED_SEMANTIC=0.0.0
      -DAXK_VERSION_EXPECTED_PROJECT=0.0.0
      "-DAXK_VERSION_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${AXK_BUILD_INFO_GENERATOR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_error}")
    message(FATAL_ERROR
      "expected build-info failure matching '${expected_error}', found:\n${output}${error}")
  endif()
endfunction()

file(REMOVE_RECURSE "${AXK_TEST_ROOT}")
file(MAKE_DIRECTORY "${AXK_TEST_ROOT}")
set(repository "${AXK_TEST_ROOT}/repository")
file(MAKE_DIRECTORY "${repository}")
run_git("${repository}" init -b main)
run_git("${repository}" config user.email test@example.invalid)
run_git("${repository}" config user.name "axklib test")
file(WRITE "${repository}/tracked.txt" "tracked\n")
run_git("${repository}" add tracked.txt)
run_git("${repository}" commit -m initial)

set(output_cpp "${AXK_TEST_ROOT}/output/build_info.cpp")
set(output_package "${AXK_TEST_ROOT}/output/package_basename.txt")
set(output_metadata "${AXK_TEST_ROOT}/output/version_metadata.json")
generate("${repository}" "${output_cpp}" "${output_package}" "${output_metadata}")
file(READ "${output_cpp}" initial_cpp)
file(READ "${output_package}" initial_package)
file(READ "${output_metadata}" initial_metadata)
string(STRIP "${initial_package}" initial_package_basename)
if(NOT initial_package STREQUAL "${initial_package_basename}\n" OR
   NOT initial_package_basename MATCHES "^axklib-main-[0-9a-f]+$")
  message(FATAL_ERROR "unexpected initial package basename: ${initial_package}")
endif()
file(TIMESTAMP "${output_cpp}" initial_cpp_timestamp "%s")
file(TIMESTAMP "${output_package}" initial_package_timestamp "%s")
file(TIMESTAMP "${output_metadata}" initial_metadata_timestamp "%s")

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
generate("${repository}" "${output_cpp}" "${output_package}" "${output_metadata}")
file(TIMESTAMP "${output_cpp}" repeated_cpp_timestamp "%s")
file(TIMESTAMP "${output_package}" repeated_package_timestamp "%s")
file(TIMESTAMP "${output_metadata}" repeated_metadata_timestamp "%s")
if(NOT initial_cpp_timestamp STREQUAL repeated_cpp_timestamp OR
   NOT initial_package_timestamp STREQUAL repeated_package_timestamp OR
   NOT initial_metadata_timestamp STREQUAL repeated_metadata_timestamp)
  message(FATAL_ERROR "unchanged metadata rewrote a generated file")
endif()

file(WRITE "${repository}/untracked.txt" "dirty\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
generate("${repository}" "${output_cpp}" "${output_package}" "${output_metadata}")
file(READ "${output_cpp}" dirty_cpp)
file(READ "${output_package}" dirty_package)
if(initial_cpp STREQUAL dirty_cpp OR NOT dirty_cpp MATCHES "-mod")
  message(FATAL_ERROR "dirty source did not refresh generated C++ metadata")
endif()
string(STRIP "${dirty_package}" dirty_package_basename)
if(NOT dirty_package STREQUAL "${dirty_package_basename}\n" OR
   NOT dirty_package_basename MATCHES "-mod$")
  message(FATAL_ERROR "dirty source did not refresh package basename")
endif()

file(REMOVE "${repository}/untracked.txt")
generate("${repository}" "${output_cpp}" "${output_package}" "${output_metadata}")
file(READ "${output_cpp}" restored_cpp)
file(READ "${output_package}" restored_package)
if(NOT restored_cpp STREQUAL initial_cpp OR NOT restored_package STREQUAL initial_package)
  message(FATAL_ERROR "clean source did not restore generated metadata")
endif()
file(READ "${output_metadata}" restored_metadata)
if(NOT restored_metadata STREQUAL initial_metadata)
  message(FATAL_ERROR "clean source changed version metadata")
endif()

run_git("${repository}" checkout --detach)
run_git("${repository}" tag v1.2.3)
expect_generation_failure(
  "${repository}"
  "${output_cpp}"
  "${output_package}"
  "${output_metadata}"
  "rerun CMake"
)
