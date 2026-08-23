cmake_minimum_required(VERSION 3.22.1...3.28)

foreach(required_variable IN ITEMS AXK_BUILD_INFO_MODULE AXK_TEST_ROOT)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

find_package(Git REQUIRED)
include("${AXK_BUILD_INFO_MODULE}")

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
  set(AXK_TEST_GIT_OUTPUT "${output}" PARENT_SCOPE)
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
run_git("${repository}" rev-parse --short=7 HEAD)
set(short_sha "${AXK_TEST_GIT_OUTPUT}")

set(AXK_GIT_EXECUTABLE "${GIT_EXECUTABLE}")
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "main-${short_sha}" "named branch identity")
assert_equal("${AXK_PACKAGE_BASENAME}" "axklib-main-${short_sha}" "named branch package")
assert_equal("${AXK_GIT_BRANCH}" main "named branch")
assert_false("${AXK_IS_TAGGED_RELEASE}" "named branch tag state")
assert_false("${AXK_GIT_DIRTY}" "clean named branch")

set(version_branches
  9.8.7
  v9.8.7
  release/9.8.7
  release/v9.8.7
  feature/9.8.7
  feature/v9.8.7
  features/9.8.7
  features/v9.8.7
  bugfix/9.8.7
  bugfix/v9.8.7
  bugfixes/9.8.7
  bugfixes/v9.8.7
)
foreach(version_branch IN LISTS version_branches)
  run_git("${repository}" checkout -B "${version_branch}" main)
  axk_derive_git_build_info("${repository}" axklib)
  assert_equal("${AXK_SOURCE_IDENTITY}" "9.8.7-pre-${short_sha}" "version branch identity")
  assert_equal("${AXK_PACKAGE_BASENAME}" "axklib-9.8.7-pre-${short_sha}" "version branch package")
endforeach()

run_git("${repository}" checkout -B feature/9.8.7 main)
file(WRITE "${repository}/untracked.txt" "dirty\n")
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "9.8.7-pre-dirty-${short_sha}" "dirty version branch identity")
assert_equal("${AXK_PACKAGE_BASENAME}" "axklib-9.8.7-pre-dirty-${short_sha}" "dirty version branch package")
assert_true("${AXK_GIT_DIRTY}" "dirty version branch")
file(REMOVE "${repository}/untracked.txt")

run_git("${repository}" checkout -B hotfix/9.8.7 main)
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "hotfix-9.8.7-${short_sha}" "non-version namespace identity")
run_git("${repository}" checkout main)

file(WRITE "${repository}/untracked.txt" "dirty\n")
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "main-dirty-${short_sha}" "dirty identity")
assert_equal("${AXK_PACKAGE_BASENAME}" "axklib-main-dirty-${short_sha}" "dirty package")
assert_true("${AXK_GIT_DIRTY}" "untracked file dirtiness")
file(REMOVE "${repository}/untracked.txt")

run_git("${repository}" tag preview)
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "main-${short_sha}" "branch wins over tag")
assert_false("${AXK_IS_TAGGED_RELEASE}" "branch wins tag state")

run_git("${repository}" checkout --detach)
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "detached-${short_sha}" "non-version exact tag")
assert_equal("${AXK_GIT_TAG}" "" "non-version tag value")
assert_false("${AXK_IS_TAGGED_RELEASE}" "non-version tag state")

run_git("${repository}" tag -d preview)
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "detached-${short_sha}" "detached identity")
assert_equal("${AXK_GIT_BRANCH}" detached "detached branch")

set(ENV{GITHUB_REF_TYPE} branch)
set(ENV{GITHUB_REF_NAME} "features/v9.8.7")
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "9.8.7-pre-${short_sha}" "GitHub version branch identity")
assert_equal("${AXK_GIT_BRANCH}" features-v9.8.7 "GitHub version branch")
assert_false("${AXK_IS_TAGGED_RELEASE}" "GitHub version branch tag state")
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})

set(ENV{GITHUB_REF_TYPE} tag)
set(ENV{GITHUB_REF_NAME} "nightly/test")
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "nightly-test-${short_sha}" "GitHub tag identity")
assert_equal("${AXK_GIT_TAG}" nightly-test "GitHub tag value")
assert_equal("${AXK_GIT_BRANCH}" nightly-test "GitHub tag branch fallback")
assert_true("${AXK_IS_TAGGED_RELEASE}" "GitHub tag state")
unset(ENV{GITHUB_REF_TYPE})
unset(ENV{GITHUB_REF_NAME})

run_git("${repository}" tag -a v1.2.3 -m release)
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "v1.2.3-${short_sha}" "annotated exact tag")
assert_equal("${AXK_GIT_TAG}" v1.2.3 "annotated tag value")

run_git("${repository}" tag -d v1.2.3)
run_git("${repository}" tag -a 1.2.3 -m release)
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" "1.2.3-${short_sha}" "unprefixed exact tag")
assert_equal("${AXK_GIT_TAG}" 1.2.3 "unprefixed exact tag value")

if(DEFINED ENV{RUNNER_TEMP} AND NOT "$ENV{RUNNER_TEMP}" STREQUAL "")
  set(external_temporary_root "$ENV{RUNNER_TEMP}")
elseif(WIN32 AND DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
  set(external_temporary_root "$ENV{TEMP}")
else()
  set(external_temporary_root "/tmp")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef temporary_suffix)
set(non_repository "${external_temporary_root}/axklib-non-repository-${temporary_suffix}")
file(MAKE_DIRECTORY "${non_repository}")
axk_derive_git_build_info("${non_repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" detached-unknown "non-repository identity")
assert_false("${AXK_GIT_DIRTY}" "non-repository status failure")
file(REMOVE_RECURSE "${non_repository}")

set(AXK_GIT_EXECUTABLE "")
axk_derive_git_build_info("${repository}" axklib)
assert_equal("${AXK_SOURCE_IDENTITY}" local-unknown "missing Git identity")
assert_equal("${AXK_GIT_BRANCH}" local "missing Git branch")
assert_false("${AXK_GIT_DIRTY}" "missing Git dirtiness")

axk_sanitize_git_ref("feature///audio import" sanitized)
assert_equal("${sanitized}" feature-audio-import "ref sanitization")
axk_sanitize_git_ref("--preview--" sanitized)
assert_equal("${sanitized}" preview "hyphen trimming")
axk_sanitize_git_ref("音色/試験" sanitized)
assert_equal("${sanitized}" unknown "Unicode sanitization")
