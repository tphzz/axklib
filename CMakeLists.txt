cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_SOURCE_DIR}/library/cmake/AxkVcpkgDefaults.cmake")

# These options must exist before project() loads the vcpkg toolchain. They
# select manifest features without making CLI or test dependencies part of a
# library-only configuration.
option(AXK_BUILD_CLI "Build the native axklib CLI" ON)
option(AXK_BUILD_SHARED_SDK "Build the public axklib shared SDK" ON)
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

file(STRINGS "${CMAKE_BINARY_DIR}/package_basename.txt" _axk_package_basename LIMIT_COUNT 1)
if(WIN32)
  set(_axk_package_platform windows)
elseif(APPLE)
  set(_axk_package_platform macos)
else()
  set(_axk_package_platform linux)
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _axk_package_processor)
if(_axk_package_processor MATCHES "^(amd64|x86_64)$")
  set(_axk_package_processor x64)
elseif(_axk_package_processor MATCHES "^(aarch64|arm64)$")
  set(_axk_package_processor arm64)
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" AND
   CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
  set(_axk_package_processor universal)
endif()
set(_axk_package_debug_suffix "")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_axk_package_debug_suffix "-debug")
endif()

set(CPACK_PACKAGE_NAME "axklib")
set(CPACK_PACKAGE_VENDOR "axklib contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "${_axk_package_basename}-${_axk_package_platform}-${_axk_package_processor}${_axk_package_debug_suffix}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL sdk cli)
set(CPACK_COMPONENT_SDK_DISPLAY_NAME "axklib C++17 shared SDK")
set(CPACK_COMPONENT_CLI_DISPLAY_NAME "axklib self-contained CLI")
include(CPack)

unset(_axk_package_basename)
unset(_axk_package_platform)
unset(_axk_package_processor)
unset(_axk_package_debug_suffix)
