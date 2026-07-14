cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AxkVcpkgDefaults.cmake")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
  option(AXK_BUILD_SHARED_SDK "Build the public axklib shared SDK" ON)
  option(BUILD_TESTING "Build the native axklib tests" ON)
  option(AXK_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)
  option(AXK_ENABLE_THREAD_SANITIZER "Enable the thread sanitizer" OFF)
  option(AXK_ENABLE_COVERAGE "Enable native source coverage instrumentation" OFF)
  option(AXK_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
  option(AXK_BUILD_BENCHMARKS "Build native workflow benchmark tools" OFF)
  option(AXK_WARNINGS_AS_ERRORS "Treat native project warnings as errors" ON)
  set(AXK_FUZZ_SMOKE_SECONDS 5 CACHE STRING "Per-target duration for fuzz smoke tests")
  set(AXK_SBOM_FILE "" CACHE FILEPATH "Optional SPDX JSON document to install")
  set(AXK_BUILD_CLI OFF CACHE BOOL "Build the native axklib CLI" FORCE)
  set(VCPKG_MANIFEST_DIR "${AXK_REPOSITORY_ROOT}")
  set(_axk_vcpkg_features)
  if(BUILD_TESTING)
    list(APPEND _axk_vcpkg_features tests)
  endif()
  set(VCPKG_MANIFEST_FEATURES "${_axk_vcpkg_features}" CACHE STRING
      "axklib library vcpkg manifest features" FORCE)
  unset(_axk_vcpkg_features)

  project(
    axklib_library
    VERSION 0.1.0
    DESCRIPTION "Yamaha A-series disk image and sampler object library"
    LANGUAGES CXX
  )
  include(CTest)
  include(GNUInstallDirs)
endif()

include(CMakePackageConfigHelpers)

if(NOT DEFINED AXK_REPOSITORY_ROOT)
  get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/AxkSanitizers.cmake)
include(cmake/AxkCoverage.cmake)
include(cmake/AxkWarnings.cmake)

set(AXK_BUILD_INFO_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/src/build_info.cpp")
set(AXK_PACKAGE_BASENAME_FILE "${CMAKE_BINARY_DIR}/package_basename.txt")
set(AXK_BUILD_INFO_MODULE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AxkGitBuildInfo.cmake")
set(AXK_BUILD_INFO_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake")
set(AXK_BUILD_INFO_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cpp.in")
set(_axk_build_info_command
  "${CMAKE_COMMAND}"
  "-DAXK_VERSION_SOURCE_DIR=${AXK_REPOSITORY_ROOT}"
  "-DAXK_VERSION_PRODUCT_NAME=axklib"
  "-DAXK_VERSION_MODULE=${AXK_BUILD_INFO_MODULE}"
  "-DAXK_VERSION_TEMPLATE=${AXK_BUILD_INFO_TEMPLATE}"
  "-DAXK_VERSION_OUTPUT_CPP=${AXK_BUILD_INFO_CPP}"
  "-DAXK_VERSION_OUTPUT_PACKAGE=${AXK_PACKAGE_BASENAME_FILE}"
  -P "${AXK_BUILD_INFO_GENERATOR}"
)
execute_process(COMMAND ${_axk_build_info_command} RESULT_VARIABLE _axk_build_info_result)
if(NOT _axk_build_info_result EQUAL 0)
  message(FATAL_ERROR "failed to generate axklib build metadata")
endif()
add_custom_target(
  axk_generate_build_info ALL
  COMMAND ${_axk_build_info_command}
  BYPRODUCTS "${AXK_BUILD_INFO_CPP}" "${AXK_PACKAGE_BASENAME_FILE}"
  VERBATIM
)
unset(_axk_build_info_command)
unset(_axk_build_info_result)

configure_file(
  include/axklib/sdk/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include/axklib/sdk/version.hpp"
  @ONLY
)

add_library(
  axk_core
  STATIC
  src/audio.cpp
  src/audio_export.cpp
  src/bytes.cpp
  src/catalog.cpp
  src/error.cpp
  src/effects.cpp
  src/io.cpp
  src/media.cpp
  src/media_fat12.cpp
  src/media_iso9660.cpp
  src/media_yamaha.cpp
  src/object.cpp
  src/package.cpp
  src/package_archive.cpp
  src/package_import.cpp
  src/package_relocation.cpp
  src/relationship.cpp
  src/report.cpp
  src/semantic.cpp
  src/sfs.cpp
  src/utf8.cpp
  src/version.cpp
  "${AXK_BUILD_INFO_CPP}"
)
add_library(axklib::core ALIAS axk_core)
add_dependencies(axk_core axk_generate_build_info)

target_compile_features(axk_core PUBLIC cxx_std_23)

if(AXK_BUILD_BENCHMARKS)
  add_executable(axk_workflow_benchmark benchmarks/workflow_benchmark.cpp)
  target_compile_features(axk_workflow_benchmark PRIVATE cxx_std_23)
  target_link_libraries(axk_workflow_benchmark PRIVATE axklib::audio nlohmann_json::nlohmann_json)
  axk_set_project_warnings(axk_workflow_benchmark)
endif()
target_include_directories(
  axk_core
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/internal"
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(
  axk_core
  PROPERTIES
    CXX_EXTENSIONS OFF
    EXPORT_NAME core
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN ON
    CXX_VISIBILITY_PRESET hidden
)
axk_set_project_warnings(axk_core)
axk_enable_sanitizers(axk_core)
axk_enable_coverage(axk_core)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)
target_link_libraries(axk_core PRIVATE nlohmann_json::nlohmann_json utf8::cpp)

add_library(
  axk_audio
  STATIC
  adapters/audio_channels.cpp
  adapters/audio_decode_sndfile.cpp
  adapters/audio_import.cpp
  adapters/audio_resample_soxr.cpp
  adapters/pcm16_quantizer.cpp
  src/alteration.cpp
  src/media_build.cpp
  src/media_write_fat12.cpp
  src/media_write_iso9660.cpp
  src/writer.cpp
  src/writer_image.cpp
)
add_library(axklib::audio ALIAS axk_audio)
target_compile_features(axk_audio PUBLIC cxx_std_23)
target_include_directories(
  axk_audio
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/internal"
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(
  axk_audio
  PROPERTIES
    CXX_EXTENSIONS OFF
    EXPORT_NAME audio
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN ON
    CXX_VISIBILITY_PRESET hidden
)
find_package(SndFile CONFIG REQUIRED)
find_package(SOXR REQUIRED)
find_package(FatFs CONFIG REQUIRED)
target_link_libraries(
  axk_audio
  PUBLIC axklib::core
  PRIVATE SndFile::sndfile SOXR::soxr FatFs::FatFs
)
axk_set_project_warnings(axk_audio)
axk_enable_sanitizers(axk_audio)
axk_enable_coverage(axk_audio)

if(AXK_BUILD_SHARED_SDK)
  add_library(axklib_shared SHARED src/sdk.cpp)
  add_library(axklib::axklib ALIAS axklib_shared)
  target_compile_features(axklib_shared PRIVATE cxx_std_23)
  target_compile_definitions(axklib_shared PRIVATE AXK_SDK_EXPORTS)
  target_include_directories(
    axklib_shared
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/internal"
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_link_libraries(axklib_shared PRIVATE axklib::audio)
  if(APPLE)
    set(_axk_macos_exports "${CMAKE_CURRENT_BINARY_DIR}/axklib-macos-exports.txt")
    file(WRITE "${_axk_macos_exports}" "__Z*3axk*\n")
    target_link_options(axklib_shared PRIVATE
      "LINKER:-exported_symbols_list,${_axk_macos_exports}")
  elseif(UNIX)
    # Static codec archives are implementation details. Localize their global
    # symbols while retaining AXK_SDK_API facade symbols from sdk.cpp.
    target_link_options(axklib_shared PRIVATE "LINKER:--exclude-libs,ALL")
  endif()
  set_target_properties(
    axklib_shared
    PROPERTIES
      CXX_EXTENSIONS OFF
      EXPORT_NAME axklib
      OUTPUT_NAME axklib
      VERSION ${PROJECT_VERSION}
      SOVERSION ${PROJECT_VERSION_MAJOR}
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
  )
  axk_set_project_warnings(axklib_shared)
  axk_enable_sanitizers(axklib_shared)
  axk_enable_coverage(axklib_shared)

  # This is a compile contract rather than a runtime test. Keep it available
  # when BUILD_TESTING is off so cross-compiled release jobs still prove that
  # every installed SDK header is consumable as C++17.
  add_library(axk_sdk_header_tests OBJECT
    tests/sdk_header_test.cpp
    tests/sdk_result_header_test.cpp
    tests/sdk_export_header_test.cpp
    tests/sdk_build_info_header_test.cpp
    tests/sdk_version_header_test.cpp
  )
  target_compile_features(axk_sdk_header_tests PRIVATE cxx_std_17)
  target_include_directories(axk_sdk_header_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include"
  )
  set_target_properties(axk_sdk_header_tests PROPERTIES CXX_EXTENSIONS OFF)
  axk_set_project_warnings(axk_sdk_header_tests)
endif()

if(AXK_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AXK_BUILD_FUZZERS requires a Clang compiler with libFuzzer")
  endif()
  function(axk_add_fuzzer name source)
    add_executable(axk_${name}_fuzz fuzz/${source})
    set_property(GLOBAL APPEND PROPERTY AXK_FUZZ_TARGETS axk_${name}_fuzz)
    target_compile_features(axk_${name}_fuzz PRIVATE cxx_std_23)
    target_link_libraries(axk_${name}_fuzz PRIVATE axklib::audio)
    target_compile_options(axk_${name}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(axk_${name}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    axk_set_project_warnings(axk_${name}_fuzz)
  endfunction()

  add_executable(axk_sfs_seed_builder fuzz/sfs_seed_builder.cpp)
  target_compile_features(axk_sfs_seed_builder PRIVATE cxx_std_23)
  target_link_libraries(axk_sfs_seed_builder PRIVATE axklib::audio)
  axk_set_project_warnings(axk_sfs_seed_builder)

  axk_add_fuzzer(bytes bytes_fuzz.cpp)
  axk_add_fuzzer(sfs_image sfs_image_fuzz.cpp)
  axk_add_fuzzer(object_header object_header_fuzz.cpp)
  foreach(object_type smpl sbnk sbac prog sequ prf3)
    string(TOUPPER "${object_type}" object_type_tag)
    axk_add_fuzzer(object_${object_type} typed_object_fuzz.cpp)
    target_compile_definitions(
      axk_object_${object_type}_fuzz PRIVATE AXK_FUZZ_OBJECT_TYPE="${object_type_tag}"
    )
  endforeach()
  axk_add_fuzzer(fat_record media_record_fuzz.cpp)
  target_compile_definitions(axk_fat_record_fuzz PRIVATE AXK_FUZZ_MEDIA_KIND=0)
  axk_add_fuzzer(iso_record media_record_fuzz.cpp)
  target_compile_definitions(axk_iso_record_fuzz PRIVATE AXK_FUZZ_MEDIA_KIND=1)
  axk_add_fuzzer(build_manifest manifest_parser_fuzz.cpp)
  target_compile_definitions(axk_build_manifest_fuzz PRIVATE AXK_FUZZ_MANIFEST_KIND=0)
  axk_add_fuzzer(transaction manifest_parser_fuzz.cpp)
  target_compile_definitions(axk_transaction_fuzz PRIVATE AXK_FUZZ_MANIFEST_KIND=1)
  axk_add_fuzzer(package_archive package_archive_fuzz.cpp)

  add_custom_target(fuzz-local)
  get_property(fuzz_targets GLOBAL PROPERTY AXK_FUZZ_TARGETS)
  foreach(target IN LISTS fuzz_targets)
    string(REGEX REPLACE "^axk_(.*)_fuzz$" "\\1" corpus_name "${target}")
    set(working_corpus "${CMAKE_BINARY_DIR}/fuzz-corpus/${corpus_name}")
    set(seed_command)
    if(corpus_name STREQUAL "sfs_image")
      list(APPEND seed_command
           COMMAND "$<TARGET_FILE:axk_sfs_seed_builder>" "${working_corpus}/valid_minimal.hds")
    endif()
    add_custom_command(
      TARGET fuzz-local
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E rm -rf "${working_corpus}"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${corpus_name}" "${working_corpus}"
      ${seed_command}
      COMMAND "$<TARGET_FILE:${target}>" "${working_corpus}"
              "-max_total_time=${AXK_FUZZ_SMOKE_SECONDS}" -timeout=5 -rss_limit_mb=2048
              -max_len=4194304 -print_final_stats=1
      VERBATIM
    )
    add_dependencies(fuzz-local ${target})
  endforeach()
  add_dependencies(fuzz-local axk_sfs_seed_builder)
endif()

if(BUILD_TESTING)
  add_test(
    NAME Version.GitMetadata
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAXK_BUILD_INFO_MODULE=${AXK_BUILD_INFO_MODULE}"
      "-DAXK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/git-build-info-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestGitBuildInfo.cmake"
  )
  add_test(
    NAME Version.GeneratedMetadata
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAXK_BUILD_INFO_GENERATOR=${AXK_BUILD_INFO_GENERATOR}"
      "-DAXK_BUILD_INFO_MODULE=${AXK_BUILD_INFO_MODULE}"
      "-DAXK_BUILD_INFO_TEMPLATE=${AXK_BUILD_INFO_TEMPLATE}"
      "-DAXK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/generated-build-info-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestBuildInfoGeneration.cmake"
  )
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts/baseline.json" _axk_contract_baseline)
  string(JSON _axk_fixture_count LENGTH "${_axk_contract_baseline}" fixtures)
  math(EXPR _axk_fixture_last "${_axk_fixture_count} - 1")
  foreach(index RANGE 0 ${_axk_fixture_last})
    string(JSON _axk_fixture_path GET "${_axk_contract_baseline}" fixtures ${index} path)
    string(JSON _axk_fixture_digest GET "${_axk_contract_baseline}" fixtures ${index} sha256)
    file(SHA256 "${AXK_REPOSITORY_ROOT}/${_axk_fixture_path}" _axk_fixture_actual)
    if(NOT _axk_fixture_actual STREQUAL _axk_fixture_digest AND
       _axk_fixture_path MATCHES "\\.json$")
      # Git attributes pin these files to LF, but an existing Windows checkout
      # can retain CRLF worktree bytes after the attribute is introduced.
      file(READ "${AXK_REPOSITORY_ROOT}/${_axk_fixture_path}" _axk_fixture_text)
      string(REPLACE "\r\n" "\n" _axk_fixture_text "${_axk_fixture_text}")
      string(SHA256 _axk_fixture_actual "${_axk_fixture_text}")
      unset(_axk_fixture_text)
    endif()
    if(NOT _axk_fixture_actual STREQUAL _axk_fixture_digest)
      message(FATAL_ERROR
        "native contract fixture digest mismatch for ${_axk_fixture_path}: "
        "expected ${_axk_fixture_digest}, found ${_axk_fixture_actual}")
    endif()
  endforeach()
  unset(_axk_contract_baseline)
  unset(_axk_fixture_count)
  unset(_axk_fixture_last)
  unset(_axk_fixture_path)
  unset(_axk_fixture_digest)
  unset(_axk_fixture_actual)
  find_package(GTest CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  include(GoogleTest)
  add_library(axk_test_support INTERFACE)
  target_link_libraries(axk_test_support INTERFACE nlohmann_json::nlohmann_json)
  target_compile_definitions(
    axk_test_support
    INTERFACE AXK_SOURCE_ROOT="${AXK_REPOSITORY_ROOT}"
  )
  add_executable(
    axk_core_tests
    tests/bytes_test.cpp
    tests/alteration_test.cpp
    tests/audio_import_test.cpp
    tests/audio_test.cpp
    tests/audio_export_test.cpp
    tests/catalog_test.cpp
    tests/effects_test.cpp
    tests/io_test.cpp
    tests/hardening_test.cpp
    tests/media_fat12_test.cpp
    tests/media_iso9660_test.cpp
    tests/media_yamaha_test.cpp
    tests/object_test.cpp
    tests/package_archive_test.cpp
    tests/package_test.cpp
    tests/report_test.cpp
    tests/sfs_test.cpp
    tests/utf8_test.cpp
    tests/native_contract_test.cpp
    tests/version_test.cpp
    tests/writer_test.cpp
  )
  target_compile_features(axk_core_tests PRIVATE cxx_std_23)
  target_include_directories(
    axk_core_tests
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/adapters"
      "${CMAKE_CURRENT_SOURCE_DIR}/fuzz"
      "${CMAKE_CURRENT_SOURCE_DIR}/internal"
  )
  target_link_libraries(
    axk_core_tests
    PRIVATE axklib::audio axk_test_support GTest::gtest_main SndFile::sndfile
  )
  if(UNIX)
    find_program(AXK_TEST_ISOINFO NAMES isoinfo)
    find_program(AXK_TEST_XORRISO NAMES xorriso)
    if(AXK_TEST_ISOINFO)
      target_compile_definitions(axk_core_tests PRIVATE AXK_TEST_ISOINFO="${AXK_TEST_ISOINFO}")
    endif()
    if(AXK_TEST_XORRISO)
      target_compile_definitions(axk_core_tests PRIVATE AXK_TEST_XORRISO="${AXK_TEST_XORRISO}")
    endif()
  endif()
  if(AXK_BUILD_SHARED_SDK)
    target_sources(axk_core_tests PRIVATE tests/sdk_test.cpp)
    target_link_libraries(axk_core_tests PRIVATE axklib::axklib)
  endif()
  axk_set_project_warnings(axk_core_tests)
  axk_enable_sanitizers(axk_core_tests)
  axk_enable_coverage(axk_core_tests)
  gtest_discover_tests(axk_core_tests)
  add_executable(axk_effect_dump tests/effect_dump.cpp)
  target_compile_features(axk_effect_dump PRIVATE cxx_std_23)
  target_link_libraries(axk_effect_dump PRIVATE axklib::core nlohmann_json::nlohmann_json)
  axk_set_project_warnings(axk_effect_dump)
  axk_enable_sanitizers(axk_effect_dump)
  add_executable(axk_sfs_semantic_dump tests/sfs_semantic_dump.cpp)
  target_compile_features(axk_sfs_semantic_dump PRIVATE cxx_std_23)
  target_link_libraries(axk_sfs_semantic_dump PRIVATE axklib::core nlohmann_json::nlohmann_json)
  axk_set_project_warnings(axk_sfs_semantic_dump)
  axk_enable_sanitizers(axk_sfs_semantic_dump)

  add_executable(axk_media_semantic_dump tests/media_semantic_dump.cpp)
  target_compile_features(axk_media_semantic_dump PRIVATE cxx_std_23)
  target_link_libraries(axk_media_semantic_dump PRIVATE axklib::core nlohmann_json::nlohmann_json)
  axk_set_project_warnings(axk_media_semantic_dump)
  axk_enable_sanitizers(axk_media_semantic_dump)
  if(UNIX AND NOT AXK_ENABLE_SANITIZERS AND NOT AXK_ENABLE_THREAD_SANITIZER)
    add_executable(axk_large_image_memory_test tests/large_image_memory_test.cpp)
    target_compile_features(axk_large_image_memory_test PRIVATE cxx_std_23)
    target_link_libraries(axk_large_image_memory_test PRIVATE axklib::audio)
    axk_set_project_warnings(axk_large_image_memory_test)
    add_test(NAME Hardening.LargeImageMemory COMMAND axk_large_image_memory_test)
    set_tests_properties(Hardening.LargeImageMemory PROPERTIES TIMEOUT 120)
  endif()
  # A sanitizer-instrumented static archive requires sanitizer runtime flags in
  # downstream executables. Package consumption is covered by the normal build;
  # sanitizer jobs exercise the library and its native tests directly.
  if(AXK_BUILD_SHARED_SDK AND NOT AXK_ENABLE_SANITIZERS AND NOT AXK_ENABLE_THREAD_SANITIZER)
    add_test(
      NAME Package.SharedSdkExports
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAXK_SHARED_LIBRARY=$<TARGET_FILE:axklib_shared>"
        "-DAXK_NM=${CMAKE_NM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSdkExports.cmake"
    )
    add_test(
      NAME Package.BuildTreeConsumer
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAXK_LIBRARY_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DAXK_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer"
        "-DAXK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-tree-consumer-test"
        "-DAXK_TEST_FIXTURE=${AXK_REPOSITORY_ROOT}/tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"
        "-DAXK_BUILD_CONFIG=$<CONFIG>"
        "-DAXK_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunBuildTreeConsumerTest.cmake"
    )
    add_test(
      NAME Package.ExternalConsumer
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAXK_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DAXK_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer"
        "-DAXK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/consumer-test"
        "-DAXK_BUILD_CONFIG=$<CONFIG>"
        "-DAXK_GENERATOR=${CMAKE_GENERATOR}"
        "-DAXK_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DAXK_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}"
        "-DAXK_VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
        "-DAXK_VCPKG_OVERLAY_TRIPLETS=${VCPKG_OVERLAY_TRIPLETS}"
        "-DAXK_TEST_FIXTURE=${AXK_REPOSITORY_ROOT}/tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunConsumerTest.cmake"
    )
  endif()
endif()

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/axklibConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/axklibConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/axklibConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/axklib"
)

if(AXK_BUILD_SHARED_SDK)
  install(
    TARGETS axklib_shared
    EXPORT axklibTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT sdk
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT sdk NAMELINK_COMPONENT sdk
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT sdk
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(FILES include/axklib/sdk.hpp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/axklib"
          COMPONENT sdk)
  install(DIRECTORY include/axklib/sdk/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/axklib/sdk"
          COMPONENT sdk PATTERN "*.in" EXCLUDE)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/include/axklib/sdk/version.hpp"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/axklib/sdk" COMPONENT sdk)
endif()
install(FILES "${AXK_REPOSITORY_ROOT}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/axklib" COMPONENT sdk)
foreach(package nlohmann-json libsndfile soxr utfcpp libflac libogg libvorbis opus fatfs)
  install(
    FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${package}/copyright"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${package}"
    COMPONENT sdk
    OPTIONAL
  )
endforeach()
if(AXK_SBOM_FILE)
  install(FILES "${AXK_SBOM_FILE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib"
          RENAME "axklib.spdx.json" COMPONENT sdk)
endif()
if(AXK_BUILD_SHARED_SDK)
  install(
    EXPORT axklibTargets
    FILE axklibTargets.cmake
    NAMESPACE axklib::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/axklib"
    COMPONENT sdk
  )
  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/axklibConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/axklibConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/axklib"
    COMPONENT sdk
  )
  export(
    EXPORT axklibTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/axklibTargets.cmake"
    NAMESPACE axklib::
  )
endif()

configure_file(cmake/Uninstall.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/Uninstall.cmake" @ONLY)
add_custom_target(
  uninstall
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/Uninstall.cmake"
  COMMENT "Remove files listed by the most recent install"
)
