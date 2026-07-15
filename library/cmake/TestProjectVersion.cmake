cmake_minimum_required(VERSION 3.28)

foreach(required_variable IN ITEMS AXK_PROJECT_VERSION_MODULE AXK_TEST_ROOT)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

find_package(Git REQUIRED)
include("${AXK_PROJECT_VERSION_MODULE}")

if(DEFINED AXK_TEST_FAILURE_REPOSITORY)
  set(AXK_GIT_EXECUTABLE "${GIT_EXECUTABLE}")
  axk_derive_project_version("${AXK_TEST_FAILURE_REPOSITORY}")
  message(FATAL_ERROR "version resolution unexpectedly succeeded")
endif()

function(run_git repository)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" ${ARGN}
    WORKING_DIRECTORY "${repository}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "git ${ARGN} failed: ${error}")
  endif()
endfunction()

function(assert_equal actual expected label)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${label}: expected '${expected}', found '${actual}'")
  endif()
endfunction()

function(assert_false actual label)
  if(actual)
    message(FATAL_ERROR "${label}: expected false")
  endif()
endfunction()

function(assert_true actual label)
  if(NOT actual)
    message(FATAL_ERROR "${label}: expected true")
  endif()
endfunction()

function(assert_resolution semantic project major minor patch release_tag prerelease)
  assert_equal("${AXK_SEMANTIC_VERSION}" "${semantic}" "semantic version")
  assert_equal("${AXK_PROJECT_VERSION}" "${project}" "project version")
  assert_equal("${AXK_VERSION_MAJOR}" "${major}" "version major")
  assert_equal("${AXK_VERSION_MINOR}" "${minor}" "version minor")
  assert_equal("${AXK_VERSION_PATCH}" "${patch}" "version patch")
  assert_equal("${AXK_RELEASE_TAG}" "${release_tag}" "release tag")
  if(prerelease)
    assert_true("${AXK_VERSION_IS_PRERELEASE}" "prerelease state")
  else()
    assert_false("${AXK_VERSION_IS_PRERELEASE}" "prerelease state")
  endif()
endfunction()

function(expect_resolution_failure repository expected_error)
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -E env --unset=GITHUB_REF_TYPE --unset=GITHUB_REF_NAME
      "${CMAKE_COMMAND}"
      "-DAXK_PROJECT_VERSION_MODULE=${AXK_PROJECT_VERSION_MODULE}"
      "-DAXK_TEST_ROOT=${AXK_TEST_ROOT}"
      "-DAXK_TEST_FAILURE_REPOSITORY=${repository}"
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_error}")
    message(FATAL_ERROR
      "expected version-resolution failure matching '${expected_error}', found:\n${output}${error}")
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

set(AXK_GIT_EXECUTABLE "${GIT_EXECUTABLE}")
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})
axk_derive_project_version("${repository}")
assert_resolution(0.0.0 0.0.0 0 0 0 "" OFF)

run_git("${repository}" tag v9.8.7)
axk_derive_project_version("${repository}")
assert_resolution(0.0.0 0.0.0 0 0 0 "" OFF)
set(ENV{GITHUB_REF_TYPE} tag)
set(ENV{GITHUB_REF_NAME} v9.8.7)
axk_derive_project_version("${repository}")
assert_resolution(9.8.7 9.8.7 9 8 7 v9.8.7 OFF)
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})

run_git("${repository}" checkout --detach)
set(ENV{GITHUB_REF_TYPE} branch)
set(ENV{GITHUB_REF_NAME} main)
axk_derive_project_version("${repository}")
assert_resolution(0.0.0 0.0.0 0 0 0 "" OFF)
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})
axk_derive_project_version("${repository}")
assert_resolution(9.8.7 9.8.7 9 8 7 v9.8.7 OFF)
set(metadata_path "${AXK_TEST_ROOT}/version_metadata.json")
axk_write_project_version_metadata("${metadata_path}")
file(READ "${metadata_path}" metadata)
string(JSON metadata_semantic GET "${metadata}" semantic_version)
string(JSON metadata_release GET "${metadata}" is_release)
assert_equal("${metadata_semantic}" 9.8.7 "metadata semantic version")
assert_true("${metadata_release}" "metadata release state")

run_git("${repository}" tag -d v9.8.7)
run_git("${repository}" tag "v1.2.3-rc.4+build.9")
axk_derive_project_version("${repository}")
assert_resolution("1.2.3-rc.4+build.9" 1.2.3 1 2 3 "v1.2.3-rc.4+build.9" ON)

run_git("${repository}" tag -d "v1.2.3-rc.4+build.9")
run_git("${repository}" tag v01.2.3)
axk_derive_project_version("${repository}")
assert_resolution(0.0.0 0.0.0 0 0 0 "" OFF)
run_git("${repository}" tag -d v01.2.3)

set(ENV{GITHUB_REF_TYPE} tag)
set(ENV{GITHUB_REF_NAME} v2.4.6-beta.2)
run_git("${repository}" tag v2.4.6-beta.2)
axk_derive_project_version("${repository}")
assert_resolution(2.4.6-beta.2 2.4.6 2 4 6 v2.4.6-beta.2 ON)
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})

run_git("${repository}" tag v2.4.7)
expect_resolution_failure("${repository}" "multiple semantic version tags")

run_git("${repository}" tag -d v2.4.7)
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v02.4.6
    "${CMAKE_COMMAND}"
    "-DAXK_PROJECT_VERSION_MODULE=${AXK_PROJECT_VERSION_MODULE}"
    "-DAXK_TEST_ROOT=${AXK_TEST_ROOT}"
    "-DAXK_TEST_FAILURE_REPOSITORY=${repository}"
    -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE invalid_tag_result
  OUTPUT_VARIABLE invalid_tag_output
  ERROR_VARIABLE invalid_tag_error
)
if(invalid_tag_result EQUAL 0 OR
   NOT "${invalid_tag_output}${invalid_tag_error}" MATCHES "not a valid semantic version tag")
  message(FATAL_ERROR
    "expected invalid GitHub tag failure, found:\n${invalid_tag_output}${invalid_tag_error}")
endif()

file(WRITE "${repository}/second.txt" "second\n")
run_git("${repository}" add second.txt)
run_git("${repository}" commit -m second)
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v2.4.6-beta.2
    "${CMAKE_COMMAND}"
    "-DAXK_PROJECT_VERSION_MODULE=${AXK_PROJECT_VERSION_MODULE}"
    "-DAXK_TEST_ROOT=${AXK_TEST_ROOT}"
    "-DAXK_TEST_FAILURE_REPOSITORY=${repository}"
    -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE mismatched_tag_result
  OUTPUT_VARIABLE mismatched_tag_output
  ERROR_VARIABLE mismatched_tag_error
)
if(mismatched_tag_result EQUAL 0 OR
   NOT "${mismatched_tag_output}${mismatched_tag_error}" MATCHES "does not identify HEAD")
  message(FATAL_ERROR
    "expected mismatched GitHub tag failure, found:\n${mismatched_tag_output}${mismatched_tag_error}")
endif()

set(AXK_GIT_EXECUTABLE "")
axk_derive_project_version("${repository}")
assert_resolution(0.0.0 0.0.0 0 0 0 "" OFF)
