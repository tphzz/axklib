cmake_minimum_required(VERSION 3.28)
project(axklib_consumer LANGUAGES C CXX)

find_package(axklib CONFIG REQUIRED COMPONENTS core c)

add_executable(axk_consumer main.cpp)
target_compile_features(axk_consumer PRIVATE cxx_std_23)
target_link_libraries(axk_consumer PRIVATE axklib::core)

add_executable(axk_c_consumer c_main.c)
target_link_libraries(axk_c_consumer PRIVATE axklib::c)
