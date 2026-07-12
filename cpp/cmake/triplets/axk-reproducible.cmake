set(VCPKG_BUILD_TYPE release)

set(_axk_source_root "$ENV{AXK_PATH_REMAP_FROM}")
set(_axk_vcpkg_root "$ENV{VCPKG_ROOT}")
if(AXK_REPRODUCIBLE_WINDOWS)
  if(_axk_source_root)
    string(APPEND VCPKG_C_FLAGS_RELEASE " /pathmap:${_axk_source_root}=axklib")
    string(APPEND VCPKG_CXX_FLAGS_RELEASE " /pathmap:${_axk_source_root}=axklib")
  endif()
  if(_axk_vcpkg_root)
    string(APPEND VCPKG_C_FLAGS_RELEASE " /pathmap:${_axk_vcpkg_root}=vcpkg")
    string(APPEND VCPKG_CXX_FLAGS_RELEASE " /pathmap:${_axk_vcpkg_root}=vcpkg")
  endif()
else()
  if(_axk_source_root)
    string(APPEND VCPKG_C_FLAGS_RELEASE
           " -ffile-prefix-map=${_axk_source_root}=/usr/src/axklib")
    string(APPEND VCPKG_CXX_FLAGS_RELEASE
           " -ffile-prefix-map=${_axk_source_root}=/usr/src/axklib")
  endif()
  if(_axk_vcpkg_root)
    string(APPEND VCPKG_C_FLAGS_RELEASE
           " -ffile-prefix-map=${_axk_vcpkg_root}=/usr/src/vcpkg")
    string(APPEND VCPKG_CXX_FLAGS_RELEASE
           " -ffile-prefix-map=${_axk_vcpkg_root}=/usr/src/vcpkg")
  endif()
endif()
