cmake_minimum_required(VERSION 3.28)

foreach(required_variable IN ITEMS AXK_VERSION_SOURCE_DIR AXK_VERSION_PROJECT_MODULE
                                   AXK_VERSION_OUTPUT_METADATA)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

include("${AXK_VERSION_PROJECT_MODULE}")
if(DEFINED AXK_VERSION_GIT_EXECUTABLE)
  set(AXK_GIT_EXECUTABLE "${AXK_VERSION_GIT_EXECUTABLE}")
endif()
axk_derive_project_version("${AXK_VERSION_SOURCE_DIR}")
axk_write_project_version_metadata("${AXK_VERSION_OUTPUT_METADATA}")

message(STATUS "axklib semantic version: ${AXK_SEMANTIC_VERSION}")
