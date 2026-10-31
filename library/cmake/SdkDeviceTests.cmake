# Compile the private facade composition into a test executable. No test hook is
# exported by the SDK shared library or installed in the public headers.
find_package(GTest CONFIG REQUIRED)
add_executable(axk_sdk_device_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/library/tests/sdk_device_read_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/library/src/sdk.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/library/src/sdk_image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/library/src/sdk_devices.cpp"
)
target_compile_features(axk_sdk_device_tests PRIVATE cxx_std_23)
target_compile_definitions(axk_sdk_device_tests PRIVATE AXK_SDK_EXPORTS AXK_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(axk_sdk_device_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/library/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/library/internal"
  "${CMAKE_CURRENT_BINARY_DIR}/library/generated/include"
)
target_link_libraries(axk_sdk_device_tests PRIVATE axklib::application GTest::gtest_main)
axk_set_project_warnings(axk_sdk_device_tests)
axk_enable_sanitizers(axk_sdk_device_tests)
axk_enable_coverage(axk_sdk_device_tests)
include(GoogleTest)
gtest_discover_tests(axk_sdk_device_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 30)
