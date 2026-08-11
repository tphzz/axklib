# Native Linux release toolchain for the C++23 standard-library surface used by axklib.
find_program(AXK_CLANG_18 clang-18 REQUIRED)
find_program(AXK_CLANGXX_18 clang++-18 REQUIRED)

set(CMAKE_C_COMPILER "${AXK_CLANG_18}" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${AXK_CLANGXX_18}" CACHE FILEPATH "C++ compiler" FORCE)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
