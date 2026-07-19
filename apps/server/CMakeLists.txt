cmake_minimum_required(VERSION 3.28)

get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Crow CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator CONFIG REQUIRED)

set(AXK_SERVER_OPENAPI_BASE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/contracts/openapi-v1.base.json")
set(AXK_SERVER_OPENAPI_PATH "${CMAKE_CURRENT_SOURCE_DIR}/contracts/openapi-v1.json")
set(AXK_SERVER_OPENAPI_COMPATIBILITY_BASELINE
    "${CMAKE_CURRENT_SOURCE_DIR}/contracts/openapi-v1.compatibility.json")
set(AXK_SERVER_GENERATED_OPENAPI_PATH "${CMAKE_CURRENT_BINARY_DIR}/generated/openapi-v1.json")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AXK_SERVER_OPENAPI_BASE_PATH}"
                                      "${AXK_SERVER_OPENAPI_PATH}"
                                      "${AXK_SERVER_OPENAPI_COMPATIBILITY_BASELINE}"
)
file(READ "${AXK_SERVER_OPENAPI_BASE_PATH}" AXK_SERVER_OPENAPI_HEX HEX)
string(LENGTH "${AXK_SERVER_OPENAPI_HEX}" AXK_SERVER_OPENAPI_HEX_LENGTH)
math(EXPR AXK_SERVER_OPENAPI_SIZE "${AXK_SERVER_OPENAPI_HEX_LENGTH} / 2")
set(AXK_SERVER_OPENAPI_BYTES "")
set(AXK_SERVER_OPENAPI_HEX_OFFSET 0)
set(AXK_SERVER_OPENAPI_HEX_PER_LINE 64)
while(AXK_SERVER_OPENAPI_HEX_OFFSET LESS AXK_SERVER_OPENAPI_HEX_LENGTH)
  math(EXPR AXK_SERVER_OPENAPI_HEX_REMAINING
       "${AXK_SERVER_OPENAPI_HEX_LENGTH} - ${AXK_SERVER_OPENAPI_HEX_OFFSET}")
  if(AXK_SERVER_OPENAPI_HEX_REMAINING LESS AXK_SERVER_OPENAPI_HEX_PER_LINE)
    set(AXK_SERVER_OPENAPI_HEX_LINE_LENGTH "${AXK_SERVER_OPENAPI_HEX_REMAINING}")
  else()
    set(AXK_SERVER_OPENAPI_HEX_LINE_LENGTH "${AXK_SERVER_OPENAPI_HEX_PER_LINE}")
  endif()
  string(SUBSTRING "${AXK_SERVER_OPENAPI_HEX}" "${AXK_SERVER_OPENAPI_HEX_OFFSET}"
                   "${AXK_SERVER_OPENAPI_HEX_LINE_LENGTH}" AXK_SERVER_OPENAPI_HEX_LINE)
  string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1, " AXK_SERVER_OPENAPI_BYTE_LINE
                       "${AXK_SERVER_OPENAPI_HEX_LINE}")
  string(APPEND AXK_SERVER_OPENAPI_BYTES "    ${AXK_SERVER_OPENAPI_BYTE_LINE}\n")
  math(EXPR AXK_SERVER_OPENAPI_HEX_OFFSET
       "${AXK_SERVER_OPENAPI_HEX_OFFSET} + ${AXK_SERVER_OPENAPI_HEX_LINE_LENGTH}")
endwhile()
configure_file(src/embedded_contract.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_contract.cpp" @ONLY)

add_library(axk_server_contract STATIC src/contract.cpp)
target_compile_features(axk_server_contract PUBLIC cxx_std_23)
target_include_directories(axk_server_contract PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(
  axk_server_contract
  PUBLIC axklib::application nlohmann_json::nlohmann_json
  PRIVATE nlohmann_json_schema_validator::validator
)
axk_set_project_warnings(axk_server_contract)
axk_enable_sanitizers(axk_server_contract)
axk_enable_coverage(axk_server_contract)

add_executable(axk_server_contract_generator EXCLUDE_FROM_ALL src/contract_generator.cpp)
target_compile_features(axk_server_contract_generator PRIVATE cxx_std_23)
target_link_libraries(axk_server_contract_generator PRIVATE axk_server_contract)
axk_set_project_warnings(axk_server_contract_generator)
axk_enable_sanitizers(axk_server_contract_generator)
axk_enable_coverage(axk_server_contract_generator)
add_custom_command(
  OUTPUT "${AXK_SERVER_GENERATED_OPENAPI_PATH}"
  COMMAND "$<TARGET_FILE:axk_server_contract_generator>" "${AXK_SERVER_OPENAPI_BASE_PATH}"
          "${AXK_SERVER_GENERATED_OPENAPI_PATH}"
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${AXK_SERVER_GENERATED_OPENAPI_PATH}"
          "${AXK_SERVER_OPENAPI_PATH}"
  DEPENDS axk_server_contract_generator "${AXK_SERVER_OPENAPI_BASE_PATH}" "${AXK_SERVER_OPENAPI_PATH}"
  COMMENT "Generating complete axklib-server OpenAPI contract from the operation registry"
  VERBATIM
)
add_custom_target(axk_server_openapi DEPENDS "${AXK_SERVER_GENERATED_OPENAPI_PATH}")

add_library(
  axk_server_adapter
  STATIC
  src/config.cpp
  src/event_delivery_budget.cpp
  src/event_dispatcher.cpp
  src/event_tickets.cpp
  src/job_json.cpp
  src/process_lifetime.cpp
  src/request_validation.cpp
  src/server.cpp
  src/telemetry.cpp
  src/workspaces.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_contract.cpp"
)
target_compile_features(axk_server_adapter PUBLIC cxx_std_23)
target_compile_definitions(axk_server_adapter PRIVATE CROW_ENFORCE_WS_SPEC)
target_include_directories(
  axk_server_adapter
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${AXK_REPOSITORY_ROOT}/library/internal"
)
target_link_libraries(
  axk_server_adapter
  PUBLIC axklib::application axk_server_contract
  PRIVATE Crow::Crow nlohmann_json::nlohmann_json
)
if(WIN32)
  target_link_libraries(axk_server_adapter PRIVATE advapi32)
endif()
axk_set_project_warnings(axk_server_adapter)
axk_enable_sanitizers(axk_server_adapter)
axk_enable_coverage(axk_server_adapter)

if(AXK_BUILD_FUZZERS)
  add_executable(
    axk_server_request_validation_fuzz
    fuzz/request_validation_fuzz.cpp
    src/request_validation.cpp
  )
  target_compile_features(axk_server_request_validation_fuzz PRIVATE cxx_std_23)
  target_include_directories(axk_server_request_validation_fuzz
                             PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(axk_server_request_validation_fuzz PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(axk_server_request_validation_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(axk_server_request_validation_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  axk_set_project_warnings(axk_server_request_validation_fuzz)

  set(AXK_SERVER_FUZZ_CORPUS "${CMAKE_BINARY_DIR}/fuzz-corpus/server_request_validation")
  add_custom_target(
    axk_server_request_validation_fuzz_smoke
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${AXK_SERVER_FUZZ_CORPUS}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/request_validation"
            "${AXK_SERVER_FUZZ_CORPUS}"
    COMMAND "$<TARGET_FILE:axk_server_request_validation_fuzz>" "${AXK_SERVER_FUZZ_CORPUS}"
            "-max_total_time=${AXK_FUZZ_SMOKE_SECONDS}" -timeout=5 -rss_limit_mb=512
            -max_len=1048576 -print_final_stats=1
    DEPENDS axk_server_request_validation_fuzz
    VERBATIM
  )
  add_dependencies(fuzz-local axk_server_request_validation_fuzz_smoke)
endif()

add_executable(axklib_server src/main.cpp)
add_executable(axk_server ALIAS axklib_server)
set_target_properties(axklib_server PROPERTIES OUTPUT_NAME axklib-server CXX_EXTENSIONS OFF)
target_compile_features(axklib_server PRIVATE cxx_std_23)
target_link_libraries(axklib_server PRIVATE axk_server_adapter)
axk_set_project_warnings(axklib_server)
axk_enable_sanitizers(axklib_server)
axk_enable_coverage(axklib_server)

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  include(GoogleTest)
  add_executable(
    axk_server_tests
    tests/config_test.cpp
    tests/event_delivery_budget_test.cpp
    tests/event_dispatcher_test.cpp
    tests/event_tickets_test.cpp
    tests/job_json_test.cpp
    tests/process_lifetime_test.cpp
    tests/request_validation_test.cpp
    tests/server_contract_test.cpp
    tests/telemetry_test.cpp
    tests/workspaces_test.cpp
  )
  target_compile_features(axk_server_tests PRIVATE cxx_std_23)
  target_compile_definitions(axk_server_tests PRIVATE AXK_SOURCE_ROOT="${AXK_REPOSITORY_ROOT}")
  target_link_libraries(
    axk_server_tests
    PRIVATE axk_server_adapter GTest::gtest_main
  )
  target_include_directories(axk_server_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  axk_set_project_warnings(axk_server_tests)
  axk_enable_sanitizers(axk_server_tests)
  axk_enable_coverage(axk_server_tests)
  gtest_discover_tests(axk_server_tests PROPERTIES LABELS server)
  add_executable(axk_server_fixture tests/server_fixture.cpp)
  target_compile_features(axk_server_fixture PRIVATE cxx_std_23)
  target_link_libraries(axk_server_fixture PRIVATE axk_server_adapter)
  axk_set_project_warnings(axk_server_fixture)
  axk_enable_sanitizers(axk_server_fixture)
  axk_enable_coverage(axk_server_fixture)
  add_dependencies(axk_server_fixture axklib_cli)
  add_executable(axk_server_direct_benchmark tests/server_direct_benchmark.cpp)
  target_compile_features(axk_server_direct_benchmark PRIVATE cxx_std_23)
  target_link_libraries(axk_server_direct_benchmark PRIVATE axklib::application)
  axk_set_project_warnings(axk_server_direct_benchmark)
  axk_enable_sanitizers(axk_server_direct_benchmark)
  axk_enable_coverage(axk_server_direct_benchmark)
  add_test(
    NAME Server.LoopbackIntegration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server_integration_test.py"
            --server "$<TARGET_FILE:axk_server_fixture>" --cli "$<TARGET_FILE:axklib_cli>"
            --fixture "${AXK_REPOSITORY_ROOT}/tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"
  )
  set_tests_properties(Server.LoopbackIntegration PROPERTIES TIMEOUT 45 LABELS server)
  add_test(
    NAME Server.ResilienceIntegration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server_resilience_test.py"
            --server "$<TARGET_FILE:axk_server_fixture>"
            --fixture "${AXK_REPOSITORY_ROOT}/tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds"
  )
  set_tests_properties(Server.ResilienceIntegration PROPERTIES TIMEOUT 45 LABELS server)
  add_test(
    NAME Server.ParentProcessLifetime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server_parent_lifetime_test.py"
            --server "$<TARGET_FILE:axk_server_fixture>"
  )
  set_tests_properties(Server.ParentProcessLifetime PROPERTIES TIMEOUT 15 LABELS server)
  add_test(
    NAME Server.PerformanceProfile
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/server_performance_test.py"
            --server "$<TARGET_FILE:axk_server_fixture>" --direct-benchmark
            "$<TARGET_FILE:axk_server_direct_benchmark>" --report
            "${CMAKE_CURRENT_BINARY_DIR}/server-performance.json"
  )
  set_tests_properties(Server.PerformanceProfile PROPERTIES TIMEOUT 45 LABELS "performance;server")
  add_test(NAME Server.VersionSmoke COMMAND axklib_server --print-version)
  set_tests_properties(Server.VersionSmoke PROPERTIES LABELS server)
  add_test(
    NAME Server.OpenApiCompatibility
    COMMAND
      "${Python3_EXECUTABLE}" "${AXK_REPOSITORY_ROOT}/tools/python/openapi_compat.py" check
      --baseline "${AXK_SERVER_OPENAPI_COMPATIBILITY_BASELINE}" --candidate
      "${AXK_SERVER_OPENAPI_PATH}"
  )
  set_tests_properties(Server.OpenApiCompatibility PROPERTIES LABELS server)
  add_test(
    NAME Server.CrowIsolation
    COMMAND "${CMAKE_COMMAND}" -DAXK_SOURCE_ROOT=${AXK_REPOSITORY_ROOT}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCrowIsolation.cmake"
  )
  set_tests_properties(Server.CrowIsolation PROPERTIES LABELS server)
endif()

install(TARGETS axklib_server RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT server)
install(FILES "${AXK_REPOSITORY_ROOT}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/axklib" COMPONENT server)
foreach(package crow asio json-schema-validator hash-library nlohmann-json libsndfile soxr utfcpp libflac libogg
                libvorbis opus fatfs)
  install(
    FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${package}/copyright"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${package}"
    COMPONENT server
    OPTIONAL
  )
endforeach()
install(FILES "${AXK_SERVER_OPENAPI_PATH}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib/server" COMPONENT server)
install(FILES "${AXK_REPOSITORY_ROOT}/docs/axklib/server.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/axklib" COMPONENT server)
if(AXK_SERVER_SBOM_FILE)
  install(FILES "${AXK_SERVER_SBOM_FILE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/axklib"
          RENAME "axklib-server.spdx.json" COMPONENT server)
endif()
