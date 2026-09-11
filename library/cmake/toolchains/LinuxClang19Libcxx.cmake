# Local Linux release toolchain for the C++23 standard-library surface used by axklib.
find_program(AXK_CLANG_19 clang-19 REQUIRED)
find_program(AXK_CLANGXX_19 clang++-19 REQUIRED)

set(CMAKE_C_COMPILER "${AXK_CLANG_19}" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${AXK_CLANGXX_19}" CACHE FILEPATH "C++ compiler" FORCE)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
