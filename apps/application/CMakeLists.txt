cmake_minimum_required(VERSION 3.28)

get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(unofficial-hash-library CONFIG REQUIRED)

add_library(axk_application STATIC src/alteration_journal.cpp src/application_operations.cpp src/audio_operations.cpp src/audition_operations.cpp src/content_digest.cpp src/content_id.cpp src/directory_archive_operations.cpp src/download_archives.cpp
                                   src/extraction_operations.cpp src/extraction_selection.cpp src/file_operations.cpp
                                   src/filesystem.cpp src/image_sessions.cpp src/job_request_resources.cpp src/jobs.cpp
                                   src/operation_registry.cpp src/package_filename.cpp src/package_operations.cpp
                                   src/package_plan_store.cpp src/path_reservations.cpp
                                   src/private_storage.cpp src/relationship_diagnostic.cpp
                                   src/session_audio_export_operations.cpp src/system_service.cpp
                                   src/secure_random.cpp src/uploads.cpp src/validation_operations.cpp src/volume_graph.cpp
                                   src/write_operations.cpp)
add_library(axklib::application ALIAS axk_application)
target_compile_features(axk_application PUBLIC cxx_std_23)
target_include_directories(
  axk_application
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${AXK_REPOSITORY_ROOT}/library/internal"
)
target_link_libraries(
  axk_application
  PUBLIC axklib::audio nlohmann_json::nlohmann_json
  PRIVATE OpenSSL::Crypto unofficial::hash-library
)
if(WIN32)
  target_compile_definitions(axk_application PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(axk_application PRIVATE advapi32 bcrypt ntdll)
endif()
axk_set_project_warnings(axk_application)
axk_enable_sanitizers(axk_application)
axk_enable_coverage(axk_application)

include("${AXK_REPOSITORY_ROOT}/library/cmake/AxkHeaderSelfContainment.cmake")
file(GLOB_RECURSE AXK_APPLICATION_PUBLIC_HEADERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/axklib/application/*.hpp")
axk_add_header_self_containment_target(
  axk_application_header_self_containment
  CXX_STANDARD 23
  HEADERS ${AXK_APPLICATION_PUBLIC_HEADERS}
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
  LINK_LIBRARIES axklib::application
)

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)
  add_executable(axk_application_tests tests/alteration_journal_test.cpp tests/application_operations_test.cpp tests/audio_operations_test.cpp
                                       tests/content_id_test.cpp
                                       tests/download_archives_test.cpp
                                       tests/extraction_operations_test.cpp tests/file_operations_test.cpp
                                       tests/filesystem_test.cpp tests/image_sessions_test.cpp tests/jobs_test.cpp
                                       tests/operation_registry_test.cpp tests/package_operations_test.cpp
                                       tests/path_reservations_test.cpp
                                       tests/secure_random_test.cpp tests/uploads_test.cpp
                                       tests/write_operations_test.cpp)
  target_compile_features(axk_application_tests PRIVATE cxx_std_23)
  target_compile_definitions(axk_application_tests PRIVATE AXK_SOURCE_ROOT="${AXK_REPOSITORY_ROOT}")
  target_include_directories(
    axk_application_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${AXK_REPOSITORY_ROOT}/library/internal"
  )
  target_link_libraries(
    axk_application_tests
    PRIVATE axklib::application GTest::gtest_main
  )
  axk_set_project_warnings(axk_application_tests)
  axk_enable_sanitizers(axk_application_tests)
  axk_enable_coverage(axk_application_tests)
  gtest_discover_tests(axk_application_tests PROPERTIES LABELS application)
endif()
