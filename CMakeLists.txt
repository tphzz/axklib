cmake_minimum_required(VERSION 3.28)

# These options must exist before project() loads the vcpkg toolchain. They
# select manifest features without making CLI or test dependencies part of a
# library-only configuration.
option(AXK_BUILD_CLI "Build the native axklib CLI" ON)
option(AXK_BUILD_SHARED_SDK "Build the public axklib shared SDK" ON)
option(AXK_INSTALL_EMBED_ENGINE "Install private static engine archives for an embedding host" OFF)
option(BUILD_TESTING "Build the native axklib tests" ON)

set(_axk_vcpkg_features)
if(AXK_BUILD_CLI)
  list(APPEND _axk_vcpkg_features cli)
endif()
if(BUILD_TESTING)
  list(APPEND _axk_vcpkg_features tests)
endif()
set(VCPKG_MANIFEST_FEATURES "${_axk_vcpkg_features}" CACHE STRING
    "axklib vcpkg manifest features selected by build options" FORCE)
unset(_axk_vcpkg_features)

project(
  axklib
  VERSION 0.1.0
  DESCRIPTION "Yamaha A-series disk image and sampler object library"
  LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)

option(AXK_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(AXK_ENABLE_THREAD_SANITIZER "Enable the thread sanitizer" OFF)
option(AXK_ENABLE_COVERAGE "Enable native source coverage instrumentation" OFF)
option(AXK_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
set(AXK_FUZZ_SMOKE_SECONDS 5 CACHE STRING "Per-target duration for the fuzz-local smoke tier")
option(AXK_BUILD_BENCHMARKS "Build native workflow benchmark tools" OFF)
option(AXK_WARNINGS_AS_ERRORS "Treat native project warnings as errors" ON)
set(AXK_SBOM_FILE "" CACHE FILEPATH "Optional SPDX JSON document to include in SDK packages")
set(AXK_CLI_SBOM_FILE "" CACHE FILEPATH "Optional SPDX JSON document to include in CLI packages")

add_subdirectory(library)
if(AXK_BUILD_CLI)
  add_subdirectory(apps/cli)
endif()

install(DIRECTORY compliance/ DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/compliance"
        COMPONENT compliance)
install(FILES vcpkg.json .gitmodules LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/source"
        COMPONENT compliance)
install(DIRECTORY external/vcpkg/ports/libsndfile external/vcpkg/ports/soxr
        DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/source/vcpkg-ports"
        COMPONENT compliance)

execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE _axk_git_result
  OUTPUT_VARIABLE _axk_git_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _axk_git_result EQUAL 0)
  set(_axk_git_revision "unknown")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/source-revision.json"
  "{\n  \"schema_version\": \"1.0\",\n"
  "  \"repository\": \"https://github.com/tphzz/axklib\",\n"
  "  \"revision\": \"${_axk_git_revision}\",\n"
  "  \"vcpkg_revision\": \"f87344cac03158cbf1467264565f1fd36b382a24\"\n}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/source-revision.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/source" COMPONENT compliance)

set(CPACK_PACKAGE_NAME "axklib")
set(CPACK_PACKAGE_VENDOR "axklib contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "axklib-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL sdk cli compliance)
set(CPACK_COMPONENT_SDK_DISPLAY_NAME "axklib C++17 shared SDK")
set(CPACK_COMPONENT_CLI_DISPLAY_NAME "axklib self-contained CLI")
set(CPACK_COMPONENT_COMPLIANCE_DISPLAY_NAME "axklib source and compliance kit")
include(CPack)
