cmake_minimum_required(VERSION 3.28)
project(axklib_consumer LANGUAGES CXX)

find_package(axklib CONFIG REQUIRED COMPONENTS axklib)

add_executable(axk_consumer main.cpp)
target_compile_features(axk_consumer PRIVATE cxx_std_17)
target_link_libraries(axk_consumer PRIVATE axklib::axklib)
target_compile_definitions(axk_consumer PRIVATE AXK_TEST_FIXTURE="${AXK_TEST_FIXTURE}")
