cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED AXK_SOURCE_ROOT)
  message(FATAL_ERROR "AXK_SOURCE_ROOT must name the axklib source directory")
endif()

if(NOT DEFINED AXK_CLANG_FORMAT_MODE)
  set(AXK_CLANG_FORMAT_MODE check)
endif()

if(NOT AXK_CLANG_FORMAT_MODE STREQUAL "check" AND
   NOT AXK_CLANG_FORMAT_MODE STREQUAL "write")
  message(FATAL_ERROR "AXK_CLANG_FORMAT_MODE must be check or write")
endif()

find_program(AXK_CLANG_FORMAT_EXECUTABLE NAMES clang-format REQUIRED)

set(AXK_CLANG_FORMAT_CONFIG "${AXK_SOURCE_ROOT}/.clang-format")
if(NOT EXISTS "${AXK_CLANG_FORMAT_CONFIG}")
  message(FATAL_ERROR
    "clang-format configuration not found: ${AXK_CLANG_FORMAT_CONFIG}")
endif()

file(GLOB_RECURSE AXK_CLANG_FORMAT_FILES
  LIST_DIRECTORIES false
  "${AXK_SOURCE_ROOT}/library/*.c"
  "${AXK_SOURCE_ROOT}/library/*.cc"
  "${AXK_SOURCE_ROOT}/library/*.cpp"
  "${AXK_SOURCE_ROOT}/library/*.cxx"
  "${AXK_SOURCE_ROOT}/library/*.h"
  "${AXK_SOURCE_ROOT}/library/*.hh"
  "${AXK_SOURCE_ROOT}/library/*.hpp"
  "${AXK_SOURCE_ROOT}/library/*.hxx"
  "${AXK_SOURCE_ROOT}/apps/cli/*.c"
  "${AXK_SOURCE_ROOT}/apps/cli/*.cc"
  "${AXK_SOURCE_ROOT}/apps/cli/*.cpp"
  "${AXK_SOURCE_ROOT}/apps/cli/*.cxx"
  "${AXK_SOURCE_ROOT}/apps/cli/*.h"
  "${AXK_SOURCE_ROOT}/apps/cli/*.hh"
  "${AXK_SOURCE_ROOT}/apps/cli/*.hpp"
  "${AXK_SOURCE_ROOT}/apps/cli/*.hxx")

if(NOT AXK_CLANG_FORMAT_FILES)
  message(FATAL_ERROR "No maintained C or C++ files were found")
endif()

foreach(AXK_CLANG_FORMAT_FILE IN LISTS AXK_CLANG_FORMAT_FILES)
  if(AXK_CLANG_FORMAT_MODE STREQUAL "write")
    set(AXK_CLANG_FORMAT_ARGUMENTS
      "--style=file:${AXK_CLANG_FORMAT_CONFIG}"
      -i
      "${AXK_CLANG_FORMAT_FILE}")
  else()
    set(AXK_CLANG_FORMAT_ARGUMENTS
      "--style=file:${AXK_CLANG_FORMAT_CONFIG}"
      --dry-run
      --Werror
      "${AXK_CLANG_FORMAT_FILE}")
  endif()

  execute_process(
    COMMAND "${AXK_CLANG_FORMAT_EXECUTABLE}" ${AXK_CLANG_FORMAT_ARGUMENTS}
    RESULT_VARIABLE AXK_CLANG_FORMAT_RESULT
    OUTPUT_VARIABLE AXK_CLANG_FORMAT_STDOUT
    ERROR_VARIABLE AXK_CLANG_FORMAT_STDERR)
  if(NOT AXK_CLANG_FORMAT_RESULT EQUAL 0)
    message(FATAL_ERROR
      "clang-format ${AXK_CLANG_FORMAT_MODE} failed for ${AXK_CLANG_FORMAT_FILE}\n"
      "${AXK_CLANG_FORMAT_STDOUT}${AXK_CLANG_FORMAT_STDERR}")
  endif()
endforeach()

list(LENGTH AXK_CLANG_FORMAT_FILES AXK_CLANG_FORMAT_FILE_COUNT)
message(STATUS
  "clang-format ${AXK_CLANG_FORMAT_MODE} passed for ${AXK_CLANG_FORMAT_FILE_COUNT} files")
