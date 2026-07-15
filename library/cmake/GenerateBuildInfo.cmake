cmake_minimum_required(VERSION 3.28)

foreach(required_variable IN ITEMS AXK_VERSION_SOURCE_DIR AXK_VERSION_PRODUCT_NAME
                                   AXK_VERSION_MODULE AXK_VERSION_TEMPLATE
                                   AXK_VERSION_OUTPUT_CPP AXK_VERSION_OUTPUT_PACKAGE)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

include("${AXK_VERSION_MODULE}")
if(DEFINED AXK_VERSION_GIT_EXECUTABLE)
  set(AXK_GIT_EXECUTABLE "${AXK_VERSION_GIT_EXECUTABLE}")
endif()
axk_derive_git_build_info("${AXK_VERSION_SOURCE_DIR}" "${AXK_VERSION_PRODUCT_NAME}")

if(AXK_IS_TAGGED_RELEASE)
  set(AXK_IS_TAGGED_RELEASE_CPP true)
else()
  set(AXK_IS_TAGGED_RELEASE_CPP false)
endif()
if(AXK_GIT_DIRTY)
  set(AXK_GIT_DIRTY_CPP true)
else()
  set(AXK_GIT_DIRTY_CPP false)
endif()

get_filename_component(output_cpp_directory "${AXK_VERSION_OUTPUT_CPP}" DIRECTORY)
get_filename_component(output_package_directory "${AXK_VERSION_OUTPUT_PACKAGE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_cpp_directory}" "${output_package_directory}")

set(output_cpp_temporary "${AXK_VERSION_OUTPUT_CPP}.tmp")
configure_file("${AXK_VERSION_TEMPLATE}" "${output_cpp_temporary}" @ONLY)
file(COPY_FILE "${output_cpp_temporary}" "${AXK_VERSION_OUTPUT_CPP}" ONLY_IF_DIFFERENT)
file(REMOVE "${output_cpp_temporary}")

set(package_text "${AXK_PACKAGE_BASENAME}\n")
set(write_package_file ON)
if(EXISTS "${AXK_VERSION_OUTPUT_PACKAGE}")
  file(READ "${AXK_VERSION_OUTPUT_PACKAGE}" existing_package_text)
  if(existing_package_text STREQUAL package_text)
    set(write_package_file OFF)
  endif()
endif()
if(write_package_file)
  file(WRITE "${AXK_VERSION_OUTPUT_PACKAGE}" "${package_text}")
endif()

message(STATUS "axklib source identity: ${AXK_SOURCE_IDENTITY}")
