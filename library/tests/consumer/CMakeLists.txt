cmake_minimum_required(VERSION 3.22.1...3.28)
project(axklib_consumer LANGUAGES CXX)

find_package(axklib CONFIG REQUIRED COMPONENTS axklib)

add_executable(axk_consumer main.cpp)
target_compile_features(axk_consumer PRIVATE cxx_std_17)
target_link_libraries(axk_consumer PRIVATE axklib::axklib)
target_compile_definitions(axk_consumer PRIVATE AXK_TEST_FIXTURE="${AXK_TEST_FIXTURE}")

add_executable(axk_set_consumer source_set.cpp)
set_target_properties(axk_set_consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(axk_set_consumer PRIVATE axklib::axklib)

add_executable(axk_convert_consumer convert.cpp)
set_target_properties(axk_convert_consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(axk_convert_consumer PRIVATE axklib::axklib)
