include_guard(GLOBAL)

get_filename_component(_axk_default_repository_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(NOT DEFINED VCPKG_OVERLAY_TRIPLETS)
  set(VCPKG_OVERLAY_TRIPLETS
      "${_axk_default_repository_root}/library/cmake/triplets"
      CACHE STRING "axklib vcpkg overlay triplets")
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(_axk_host_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  if(NOT _axk_host_processor)
    cmake_host_system_information(RESULT _axk_host_processor QUERY OS_PLATFORM)
  endif()
  if(NOT _axk_host_processor AND DEFINED ENV{PROCESSOR_ARCHITECTURE})
    set(_axk_host_processor "$ENV{PROCESSOR_ARCHITECTURE}")
  endif()
  string(TOLOWER "${_axk_host_processor}" _axk_host_processor)

  if(_axk_host_processor MATCHES "^(amd64|x86_64)$")
    set(_axk_triplet_architecture x64)
  elseif(_axk_host_processor MATCHES "^(arm64|aarch64)$")
    set(_axk_triplet_architecture arm64)
  endif()

  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(_axk_triplet_system windows)
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(_axk_triplet_system linux)
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(_axk_triplet_system osx)
  endif()

  if(DEFINED _axk_triplet_architecture AND DEFINED _axk_triplet_system)
    set(VCPKG_TARGET_TRIPLET
        "${_axk_triplet_architecture}-${_axk_triplet_system}-axk"
        CACHE STRING "axklib default vcpkg target triplet")
  else()
    message(STATUS
      "No axklib default vcpkg triplet for host "
      "${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}; specify VCPKG_TARGET_TRIPLET")
  endif()
endif()

unset(_axk_default_repository_root)
unset(_axk_host_processor)
unset(_axk_triplet_architecture)
unset(_axk_triplet_system)
