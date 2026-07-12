include(CMakePackageConfigHelpers)

include(cmake/AxkSanitizers.cmake)
include(cmake/AxkCoverage.cmake)
include(cmake/AxkWarnings.cmake)

add_library(
  axk_core
  src/audio.cpp
  src/audio_export.cpp
  src/bytes.cpp
  src/catalog.cpp
  src/error.cpp
  src/effects.cpp
  src/io.cpp
  src/media.cpp
  src/object.cpp
  src/relationship.cpp
  src/report.cpp
  src/semantic.cpp
  src/sfs.cpp
  src/version.cpp
)
add_library(axklib::core ALIAS axk_core)

target_compile_features(axk_core PUBLIC cxx_std_23)
target_compile_definitions(axk_core PRIVATE AXK_CORE_EXPORTS)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(axk_core PUBLIC AXK_SHARED_LIBRARY)
endif()

if(AXK_BUILD_BENCHMARKS)
  add_executable(axk_workflow_benchmark benchmarks/workflow_benchmark.cpp)
  target_compile_features(axk_workflow_benchmark PRIVATE cxx_std_23)
  target_link_libraries(axk_workflow_benchmark PRIVATE axklib::audio nlohmann_json::nlohmann_json)
  axk_set_project_warnings(axk_workflow_benchmark)
endif()
target_include_directories(
  axk_core
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
target_link_libraries(axk_core PRIVATE nlohmann_json::nlohmann_json)

add_library(
  axk_audio
  adapters/audio_import.cpp
  src/alteration.cpp
  src/writer.cpp
  src/writer_image.cpp
)
add_library(axklib::audio ALIAS axk_audio)
target_compile_features(axk_audio PUBLIC cxx_std_23)

if(APPLE)
  set_target_properties(axk_audio PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path"
  )
elseif(UNIX)
  set_target_properties(axk_audio PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
  )
endif()
target_include_directories(
  axk_audio
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
find_package(PkgConfig REQUIRED)
pkg_check_modules(SOXR REQUIRED IMPORTED_TARGET soxr)
target_link_libraries(
  axk_audio
  PUBLIC axklib::core
  PRIVATE SndFile::sndfile PkgConfig::SOXR
)
axk_set_project_warnings(axk_audio)
axk_enable_sanitizers(axk_audio)
axk_enable_coverage(axk_audio)

add_library(axk_c SHARED src/c_api.cpp)
add_library(axklib::c ALIAS axk_c)
target_compile_features(axk_c PRIVATE cxx_std_23)
target_compile_definitions(axk_c PRIVATE AXK_C_EXPORTS PUBLIC AXK_SHARED_LIBRARY)
target_include_directories(
  axk_c
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(axk_c PRIVATE axklib::audio)
set_target_properties(
  axk_c
  PROPERTIES
    CXX_EXTENSIONS OFF
    EXPORT_NAME c
    OUTPUT_NAME axklib_c
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(UNIX AND NOT APPLE)
  target_link_options(
    axk_c PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/abi/axklib_c.map"
  )
elseif(APPLE)
  target_link_options(
    axk_c PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/abi/axklib_c.exports"
  )
elseif(WIN32)
  target_sources(axk_c PRIVATE abi/axklib_c.def)
endif()
axk_set_project_warnings(axk_c)
axk_enable_sanitizers(axk_c)
axk_enable_coverage(axk_c)

if(AXK_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AXK_BUILD_FUZZERS requires a Clang compiler with libFuzzer")
  endif()
  function(axk_add_fuzzer name source)
    add_executable(axk_${name}_fuzz fuzz/${source})
    target_compile_features(axk_${name}_fuzz PRIVATE cxx_std_23)
    target_link_libraries(axk_${name}_fuzz PRIVATE axklib::audio)
    target_compile_options(axk_${name}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(axk_${name}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    axk_set_project_warnings(axk_${name}_fuzz)
  endfunction()

  axk_add_fuzzer(bytes bytes_fuzz.cpp)
  axk_add_fuzzer(sfs_superblock sfs_sections_fuzz.cpp)
  target_compile_definitions(axk_sfs_superblock_fuzz PRIVATE AXK_FUZZ_SECTION=0)
  axk_add_fuzzer(sfs_index sfs_sections_fuzz.cpp)
  target_compile_definitions(axk_sfs_index_fuzz PRIVATE AXK_FUZZ_SECTION=1)
  axk_add_fuzzer(sfs_directory sfs_sections_fuzz.cpp)
  target_compile_definitions(axk_sfs_directory_fuzz PRIVATE AXK_FUZZ_SECTION=2)
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
endif()

if(AXK_BUILD_CLI)
  find_package(CLI11 CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  add_library(axk_cli_adapter STATIC cli/app.cpp)
  target_compile_features(axk_cli_adapter PUBLIC cxx_std_23)
  target_include_directories(axk_cli_adapter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cli")
  target_link_libraries(
    axk_cli_adapter PRIVATE axklib::audio CLI11::CLI11 nlohmann_json::nlohmann_json
  )
  axk_set_project_warnings(axk_cli_adapter)
  axk_enable_sanitizers(axk_cli_adapter)
  axk_enable_coverage(axk_cli_adapter)
  add_executable(axklib_cli cli/main.cpp)
  add_executable(axk_cli ALIAS axklib_cli)
  set_target_properties(axklib_cli PROPERTIES OUTPUT_NAME axklib CXX_EXTENSIONS OFF)
  target_compile_features(axklib_cli PRIVATE cxx_std_23)
  target_link_libraries(
    axklib_cli PRIVATE axk_cli_adapter
  )
  axk_set_project_warnings(axklib_cli)
  axk_enable_sanitizers(axklib_cli)
  axk_enable_coverage(axklib_cli)
endif()

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  find_package(nlohmann_json CONFIG REQUIRED)
  include(GoogleTest)
  add_library(axk_test_support INTERFACE)
  target_link_libraries(axk_test_support INTERFACE nlohmann_json::nlohmann_json)
  target_compile_definitions(
    axk_test_support
    INTERFACE AXK_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
  )
  add_executable(
    axk_core_tests
    tests/bytes_test.cpp
    tests/alteration_test.cpp
    tests/audio_test.cpp
    tests/audio_export_test.cpp
    tests/catalog_test.cpp
    tests/c_api_test.cpp
    tests/effects_test.cpp
    tests/io_test.cpp
    tests/hardening_test.cpp
    tests/media_test.cpp
    tests/object_test.cpp
    tests/report_test.cpp
    tests/sfs_test.cpp
    tests/oracle_baseline_test.cpp
    tests/version_test.cpp
    tests/writer_test.cpp
  )
  target_compile_features(axk_core_tests PRIVATE cxx_std_23)
  target_link_libraries(
    axk_core_tests
    PRIVATE axklib::audio axklib::c axk_test_support GTest::gtest_main
  )
  if(AXK_BUILD_CLI)
    target_sources(axk_core_tests PRIVATE tests/cli_parser_test.cpp)
    target_link_libraries(axk_core_tests PRIVATE axk_cli_adapter)
  endif()
  axk_set_project_warnings(axk_core_tests)
  axk_enable_sanitizers(axk_core_tests)
  axk_enable_coverage(axk_core_tests)
  gtest_discover_tests(axk_core_tests)
  add_executable(axk_c_consumer_test tests/c_consumer.c)
  set_target_properties(axk_c_consumer_test PROPERTIES LINKER_LANGUAGE CXX)
  target_link_libraries(axk_c_consumer_test PRIVATE axklib::c)
  axk_set_project_warnings(axk_c_consumer_test)
  axk_enable_sanitizers(axk_c_consumer_test)
  foreach(example inventory exact_export create alter)
    add_executable(axk_c_example_${example} examples/c/${example}.c)
    target_link_libraries(axk_c_example_${example} PRIVATE axklib::c)
    axk_set_project_warnings(axk_c_example_${example})
    axk_enable_sanitizers(axk_c_example_${example})
  endforeach()
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
  add_test(
    NAME CApi.CConsumer
    COMMAND axk_c_consumer_test
      "${PROJECT_SOURCE_DIR}/tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"
  )
  if(UNIX AND NOT AXK_ENABLE_SANITIZERS AND NOT AXK_ENABLE_THREAD_SANITIZER)
    add_executable(axk_large_image_memory_test tests/large_image_memory_test.cpp)
    target_compile_features(axk_large_image_memory_test PRIVATE cxx_std_23)
    target_link_libraries(axk_large_image_memory_test PRIVATE axklib::audio)
    axk_set_project_warnings(axk_large_image_memory_test)
    add_test(NAME Hardening.LargeImageMemory COMMAND axk_large_image_memory_test)
    set_tests_properties(Hardening.LargeImageMemory PROPERTIES TIMEOUT 120)
  endif()
  if(UNIX)
    add_test(
      NAME CApi.ExportedSymbols
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAXK_LIBRARY=$<TARGET_FILE:axk_c>"
        "-DAXK_BASELINE=${CMAKE_CURRENT_SOURCE_DIR}/abi/axklib_c.symbols"
        "-DAXK_NM=${CMAKE_NM}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckExportedSymbols.cmake"
    )
  endif()
  # A sanitizer-instrumented static archive requires sanitizer runtime flags in
  # downstream executables. Package consumption is covered by the normal build;
  # sanitizer jobs exercise the library and its native tests directly.
  if(NOT AXK_ENABLE_SANITIZERS AND NOT AXK_ENABLE_THREAD_SANITIZER)
    add_test(
      NAME Package.ExternalConsumer
      COMMAND
        "${CMAKE_COMMAND}"
        "-DAXK_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DAXK_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer"
        "-DAXK_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/consumer-test"
        "-DAXK_BUILD_CONFIG=$<CONFIG>"
        "-DAXK_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DAXK_VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}"
        "-DAXK_VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
        "-DAXK_VCPKG_OVERLAY_TRIPLETS=${VCPKG_OVERLAY_TRIPLETS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunConsumerTest.cmake"
    )
  endif()
endif()

configure_file(cmake/axklib.pc.in axklib.pc @ONLY)

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

install(
  TARGETS axk_core axk_audio axk_c
  EXPORT axklibTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
if(AXK_BUILD_CLI)
  install(TARGETS axklib_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY examples/c/ DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/examples/c")
install(DIRECTORY examples/cpp/ DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/examples/cpp")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/axklib")
foreach(package cli11 nlohmann-json libsndfile soxr libflac libogg libvorbis mp3lame mpg123 opus)
  install(
    FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${package}/copyright"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${package}"
    OPTIONAL
  )
endforeach()
if(AXK_SBOM_FILE)
  install(FILES "${AXK_SBOM_FILE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib"
          RENAME "axklib.spdx.json")
endif()
install(
  EXPORT axklibTargets
  FILE axklibTargets.cmake
  NAMESPACE axklib::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/axklib"
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/axklibConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/axklibConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/axklib"
)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/axklib.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

configure_file(cmake/Uninstall.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/Uninstall.cmake" @ONLY)
add_custom_target(
  uninstall
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/Uninstall.cmake"
  COMMENT "Remove files listed by the most recent install"
)
