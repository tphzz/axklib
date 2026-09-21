cmake_minimum_required(VERSION 3.22.1...3.28)

get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(unofficial-hash-library CONFIG REQUIRED)
find_package(ICU REQUIRED COMPONENTS i18n uc)

add_library(axk_application STATIC src/alteration_journal.cpp src/alteration_journal_io.cpp src/application_operations.cpp src/audio_export_layout.cpp src/audio_operations.cpp src/audition_operations.cpp src/content_digest.cpp src/content_id.cpp src/corpus_audit_operations.cpp src/directory_archive_operations.cpp src/download_archives.cpp
                                   src/extraction_operations.cpp src/extraction_selection.cpp
                                   src/file_coverage_operations.cpp src/file_coverage_support.cpp
                                   src/file_info_operations.cpp src/file_inventory_operations.cpp
                                   src/file_operations.cpp src/file_operations_support.cpp
                                   src/file_relationship_operations.cpp
                                   src/filesystem.cpp src/filesystem_directory.cpp src/natural_name_order.cpp
                                   src/filesystem_directory_publication.cpp src/filesystem_entry_mutations.cpp
                                   src/filesystem_edit_operations.cpp
                                   src/filesystem_session_mutation.cpp src/system_file_operations.cpp
                                   src/filesystem_export.cpp
                                   src/filesystem_export_plan.cpp
                                   src/filesystem_export_jobs.cpp
                                   src/operation_registry_filesystem.cpp
                                   src/image_filesystem_reader.cpp
                                   src/filesystem_edit_jobs.cpp
                                   src/su700_import_roots.cpp
                                   src/su700_import_operations.cpp src/image_su700_import.cpp
                                   src/filesystem_inputs.cpp
                                   src/filesystem_image_inputs.cpp
                                   src/image_filesystem_import.cpp
                                   src/filesystem_import_inspection.cpp
                                   src/filesystem_file_publication.cpp src/filesystem_native.cpp
                                   src/filesystem_native_publication.cpp src/filesystem_paths.cpp
                                   src/filesystem_roots.cpp src/filesystem_support.cpp src/filesystem_tree.cpp
                                   src/image_session_access.cpp src/image_session_allocation_map.cpp src/image_session_audition.cpp
                                   src/image_allocation_map.cpp
                                   src/image_filesystem.cpp src/image_filesystem_index.cpp src/image_filesystem_edits.cpp
                                   src/image_filesystem_attributes.cpp
                                   src/image_session_floppy_set.cpp src/image_session_inspection.cpp
                                   src/image_session_contracts.cpp src/image_session_operations.cpp
                                   src/image_session_program_generation.cpp
                                   src/image_session_program_assignment_cleanup.cpp
                                   src/image_session_system_program_contexts.cpp
                                   src/image_session_object_detail.cpp src/image_session_object_reader.cpp src/image_session_open.cpp
                                   src/a_series_sample_editor.cpp
                                   src/a_series_bank_editor.cpp
                                   src/sample_formats.cpp
                                   src/image_session_directory.cpp
                                   src/image_session_support.cpp src/image_sessions.cpp
                                   src/job_request_resources.cpp src/jobs.cpp src/midi_operations.cpp
                                   src/jobs_support.cpp
                                   src/operation_registry.cpp src/operation_registry_names.cpp
                                   src/operation_registry_program_assignments.cpp
                                   src/package_filename.cpp src/package_operations.cpp
                                   src/package_operations_support.cpp src/session_package_import_plan.cpp
                                   src/session_package_operations.cpp
                                   src/session_import_plan.cpp
                                   src/floppy_import_operations.cpp src/floppy_import_directories.cpp
                                   src/operation_registry_floppy.cpp
                                   src/package_plan_store.cpp src/path_reservations.cpp
                                   src/private_storage.cpp src/relationship_diagnostic.cpp
                                   src/session_audio_export_operations.cpp src/session_extent_layout_repair_operations.cpp
                                   src/session_media_conversion_operations.cpp
                                   src/session_placement_operations.cpp src/session_placement_plan.cpp
                                   src/session_sequence_operations.cpp src/session_volume_package_operations.cpp
                                   src/session_tx16w_operations.cpp
                                   src/session_volume_floppy_export_operations.cpp
                                   src/system_service.cpp
                                   src/secure_random.cpp src/uploads.cpp src/validation_allocation.cpp
                                   src/validation_exports.cpp src/validation_operations.cpp
                                   src/validation_operations_support.cpp src/validation_semantics.cpp src/volume_graph.cpp
                                   src/session_write_operations.cpp src/write_manifest_operations.cpp
                                   src/write_manifest_support.cpp src/write_operations.cpp src/write_plan_support.cpp)
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
  PRIVATE OpenSSL::Crypto unofficial::hash-library ICU::i18n ICU::uc
)
if(WIN32)
  target_compile_definitions(axk_application PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(axk_application PRIVATE advapi32 bcrypt ntdll)
endif()
axk_set_project_warnings(axk_application)
axk_enable_sanitizers(axk_application)
axk_enable_coverage(axk_application)

if(AXK_BUILD_BENCHMARKS)
  add_executable(axk_files_edit_benchmark benchmarks/files_edit_benchmark.cpp)
  target_link_libraries(axk_files_edit_benchmark PRIVATE axklib::application)
  axk_set_project_warnings(axk_files_edit_benchmark)
endif()

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
  add_executable(axk_application_tests tests/alteration_journal_test.cpp tests/application_operations_test.cpp tests/audio_export_layout_test.cpp tests/audio_operations_test.cpp
                                       tests/a_series_bank_editor_test.cpp tests/content_id_test.cpp tests/image_allocation_map_test.cpp
                                       tests/download_archives_test.cpp
                                       tests/extraction_operations_test.cpp tests/export_parameter_ownership_test.cpp tests/file_operations_test.cpp
                                       tests/filesystem_test.cpp tests/natural_name_order_test.cpp tests/image_filesystem_test.cpp tests/image_sessions_test.cpp tests/jobs_test.cpp
                                       tests/filesystem_edit_operations_test.cpp
                                       tests/filesystem_fat_edit_test.cpp
                                       tests/filesystem_inputs_test.cpp
                                       tests/filesystem_image_inputs_test.cpp
                                       tests/su700_import_test.cpp
                                       tests/system_program_context_test.cpp
                                       tests/system_file_operations_test.cpp
                                       tests/filesystem_export_test.cpp
                                       tests/filesystem_export_profiles_test.cpp
                                       tests/midi_operations_test.cpp
                                       tests/operation_registry_test.cpp tests/package_operations_test.cpp
                                       tests/path_reservations_test.cpp
                                       tests/session_media_conversion_operations_test.cpp
                                       tests/session_volume_floppy_export_operations_test.cpp
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
  if(WIN32)
    target_compile_definitions(axk_application_tests PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
  endif()
  axk_set_project_warnings(axk_application_tests)
  axk_enable_sanitizers(axk_application_tests)
  axk_enable_coverage(axk_application_tests)
  gtest_discover_tests(axk_application_tests PROPERTIES LABELS application)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT AXK_ENABLE_SANITIZERS AND NOT AXK_ENABLE_THREAD_SANITIZER)
    # Sanitizers reserve large virtual address ranges incompatible with this hard address-space ceiling.
    add_executable(axk_export_memory_test tests/export_parameter_ownership_test.cpp)
    target_compile_features(axk_export_memory_test PRIVATE cxx_std_23)
    target_compile_definitions(axk_export_memory_test PRIVATE AXK_EXPORT_MEMORY_TEST)
    target_link_libraries(axk_export_memory_test PRIVATE axklib::application GTest::gtest)
    axk_set_project_warnings(axk_export_memory_test)
    axk_enable_coverage(axk_export_memory_test)
    add_test(NAME Application.ExportParameterMemory COMMAND axk_export_memory_test)
    set_tests_properties(Application.ExportParameterMemory PROPERTIES LABELS "application;memory" TIMEOUT 120)
  endif()
endif()
