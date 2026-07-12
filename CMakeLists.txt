cmake_minimum_required(VERSION 3.28)

project(
  axklib
  VERSION 0.1.0
  DESCRIPTION "Yamaha A-series disk image and sampler object library"
  LANGUAGES C CXX
)

include(CTest)
include(GNUInstallDirs)

option(AXK_BUILD_CLI "Build the native axklib CLI" ON)
option(AXK_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
option(AXK_ENABLE_THREAD_SANITIZER "Enable the thread sanitizer" OFF)
option(AXK_ENABLE_COVERAGE "Enable native source coverage instrumentation" OFF)
option(AXK_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
set(AXK_FUZZ_SMOKE_SECONDS 5 CACHE STRING "Per-target duration for the fuzz-local smoke tier")
option(AXK_BUILD_BENCHMARKS "Build native workflow benchmark tools" OFF)
option(AXK_WARNINGS_AS_ERRORS "Treat native project warnings as errors" ON)
set(AXK_SBOM_FILE "" CACHE FILEPATH "Optional SPDX JSON document to include in SDK packages")

add_subdirectory(cpp)

set(CPACK_PACKAGE_NAME "axklib")
set(CPACK_PACKAGE_VENDOR "axklib contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "axklib-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;ZIP")
include(CPack)
