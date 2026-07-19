cmake_minimum_required(VERSION 3.28)

get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${AXK_REPOSITORY_ROOT}/library/cmake/AxkVcpkgDefaults.cmake")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(BUILD_TESTING "Build native tests" ON)
  option(AXK_WARNINGS_AS_ERRORS "Treat native project warnings as errors" ON)
  option(AXK_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
  option(AXK_ENABLE_THREAD_SANITIZER "Enable the thread sanitizer" OFF)
  option(AXK_ENABLE_COVERAGE "Enable native source coverage instrumentation" OFF)
  option(AXK_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
  option(AXK_BUILD_BENCHMARKS "Build native workflow benchmark tools" OFF)
  set(AXK_FUZZ_SMOKE_SECONDS 5 CACHE STRING "Per-target duration for fuzz smoke tests")
  set(AXK_SBOM_FILE "" CACHE FILEPATH "Optional SPDX JSON document to install")
  set(AXK_CLI_SBOM_FILE "" CACHE FILEPATH "Optional SPDX JSON document to install")
  set(AXK_BUILD_CLI ON CACHE BOOL "Build the native axklib CLI" FORCE)
  set(AXK_BUILD_SHARED_SDK OFF CACHE BOOL "Build the public axklib shared SDK" FORCE)
  set(VCPKG_MANIFEST_DIR "${AXK_REPOSITORY_ROOT}")
  set(_axk_vcpkg_features cli)
  if(BUILD_TESTING)
    list(APPEND _axk_vcpkg_features tests)
  endif()
  set(VCPKG_MANIFEST_FEATURES "${_axk_vcpkg_features}" CACHE STRING
      "axklib CLI vcpkg manifest features" FORCE)
  unset(_axk_vcpkg_features)

  include("${AXK_REPOSITORY_ROOT}/library/cmake/AxkProjectVersion.cmake")
  axk_derive_project_version("${AXK_REPOSITORY_ROOT}")

  project(
    axklib_cli_project
    VERSION ${AXK_PROJECT_VERSION}
    DESCRIPTION "Standalone Yamaha A-series disk image command-line tool"
    LANGUAGES CXX
  )
  include(CTest)
  include(GNUInstallDirs)
  add_subdirectory("${AXK_REPOSITORY_ROOT}/library" "${CMAKE_BINARY_DIR}/library")
endif()

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(
  axk_cli_adapter
  STATIC
  app.cpp
  command_line.cpp
  local_operations.cpp
  commands/analysis.cpp
  commands/extract.cpp
  commands/package.cpp
  commands/reports.cpp
  commands/support.cpp
  commands/writer.cpp
  schema/export_v1.cpp
  schema/info_v1.cpp
  schema/operations_v1.cpp
  schema/package_v1.cpp
  writer_commands.cpp
)
target_compile_features(axk_cli_adapter PUBLIC cxx_std_23)
target_include_directories(
  axk_cli_adapter
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
  PRIVATE "${AXK_REPOSITORY_ROOT}/library/internal"
)
target_link_libraries(
  axk_cli_adapter
  PUBLIC axklib::core axklib::application
  PRIVATE axklib::audio CLI11::CLI11 nlohmann_json::nlohmann_json
)
axk_set_project_warnings(axk_cli_adapter)
axk_enable_sanitizers(axk_cli_adapter)
axk_enable_coverage(axk_cli_adapter)

add_executable(axklib_cli main.cpp)
add_executable(axk_cli ALIAS axklib_cli)
set_target_properties(axklib_cli PROPERTIES OUTPUT_NAME axklib CXX_EXTENSIONS OFF)
target_compile_features(axklib_cli PRIVATE cxx_std_23)
target_link_libraries(axklib_cli PRIVATE axk_cli_adapter)
if(WIN32)
  set(_axk_cli_windows_manifest "${CMAKE_CURRENT_SOURCE_DIR}/windows-utf8.manifest")
  target_link_options(
    axklib_cli
    PRIVATE "/MANIFESTINPUT:${_axk_cli_windows_manifest}"
  )
  set_property(TARGET axklib_cli APPEND PROPERTY LINK_DEPENDS "${_axk_cli_windows_manifest}")
  unset(_axk_cli_windows_manifest)
endif()
axk_set_project_warnings(axklib_cli)
axk_enable_sanitizers(axklib_cli)
axk_enable_coverage(axklib_cli)

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)
  add_executable(axk_cli_tests tests/cli_parser_test.cpp tests/cli_schema_test.cpp)
  target_compile_features(axk_cli_tests PRIVATE cxx_std_23)
  target_compile_definitions(axk_cli_tests PRIVATE AXK_SOURCE_ROOT="${AXK_REPOSITORY_ROOT}")
  target_include_directories(axk_cli_tests PRIVATE "${AXK_REPOSITORY_ROOT}/library/internal")
  target_link_libraries(
    axk_cli_tests
    PRIVATE axk_cli_adapter GTest::gtest_main nlohmann_json::nlohmann_json
  )
  if(TARGET axklib::axklib)
    target_link_libraries(axk_cli_tests PRIVATE axklib::axklib)
    target_compile_definitions(axk_cli_tests PRIVATE AXK_TEST_SHARED_SDK=1)
  endif()
  axk_set_project_warnings(axk_cli_tests)
  axk_enable_sanitizers(axk_cli_tests)
  axk_enable_coverage(axk_cli_tests)
  if(WIN32 AND TARGET axklib::axklib)
    add_custom_command(
      TARGET axk_cli_tests
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy -t "$<TARGET_FILE_DIR:axk_cli_tests>"
              "$<TARGET_RUNTIME_DLLS:axk_cli_tests>"
      COMMAND_EXPAND_LISTS
    )
  endif()
  gtest_discover_tests(axk_cli_tests)
  add_executable(axk_object_semantic_dump tests/object_semantic_dump.cpp schema/objects_v1.cpp)
  target_compile_features(axk_object_semantic_dump PRIVATE cxx_std_23)
  target_include_directories(
    axk_object_semantic_dump
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${AXK_REPOSITORY_ROOT}/library/internal"
  )
  target_link_libraries(
    axk_object_semantic_dump
    PRIVATE axklib::core nlohmann_json::nlohmann_json
  )
  axk_set_project_warnings(axk_object_semantic_dump)
  axk_enable_sanitizers(axk_object_semantic_dump)

  add_executable(axk_preview_dump tests/preview_dump.cpp)
  target_compile_features(axk_preview_dump PRIVATE cxx_std_23)
  target_link_libraries(axk_preview_dump PRIVATE axklib::audio nlohmann_json::nlohmann_json)
  axk_set_project_warnings(axk_preview_dump)
  axk_enable_sanitizers(axk_preview_dump)
  set(_axk_semantic_fixture
      "${AXK_REPOSITORY_ROOT}/tests/fixtures/images/sampler-authored/HD00_512_multi_sbnk_authored.hds"
  )
  add_test(
    NAME Development.ObjectSemanticDump
    COMMAND axk_object_semantic_dump "${_axk_semantic_fixture}"
  )
  add_test(
    NAME Development.PreviewDump
    COMMAND axk_preview_dump "${_axk_semantic_fixture}" "p0:sfs9" 257
  )
  unset(_axk_semantic_fixture)

  add_test(
    NAME Cli.UnicodeProcessPaths
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAXK_CLI=$<TARGET_FILE:axklib_cli>"
      "-DAXK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestUnicodeCli.cmake"
  )
endif()

install(TARGETS axklib_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT cli)
install(FILES "${AXK_REPOSITORY_ROOT}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/axklib" COMPONENT cli)
foreach(package cli11 hash-library nlohmann-json libsndfile soxr utfcpp libflac libogg
                libvorbis opus fatfs)
  install(
    FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${package}/copyright"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${package}"
    COMPONENT cli
    OPTIONAL
  )
endforeach()
if(AXK_CLI_SBOM_FILE)
  install(FILES "${AXK_CLI_SBOM_FILE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib"
          RENAME "axklib-cli.spdx.json" COMPONENT cli)
endif()
