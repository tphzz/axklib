get_filename_component(AXK_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(FATFS_SUBMODULE_ROOT "${AXK_REPOSITORY_ROOT}/external/fatfs")

function(axk_verify_fatfs_file relative_path expected_sha512)
  set(source_path "${FATFS_SUBMODULE_ROOT}/${relative_path}")
  if(NOT EXISTS "${source_path}")
    message(FATAL_ERROR
            "Missing FatFs source file ${source_path}. Run git submodule update --init --recursive.")
  endif()

  file(SHA512 "${source_path}" actual_sha512)
  if(NOT actual_sha512 STREQUAL expected_sha512)
    message(FATAL_ERROR
            "FatFs source hash mismatch for ${relative_path}: expected ${expected_sha512}, found ${actual_sha512}")
  endif()
endfunction()

# These files match the official R0.16 archive. The pinned submodule is a Git mirror
# used to make builds independent of the upstream archive server.
axk_verify_fatfs_file(
  "source/ff.c"
  "250439eca0f750951df7c36c37dd6bd6ae61fef65308964ce7955a1ee08023ae902a11978dae44be70cacf28ce5f1b94737f250ef3c9848aef8fa1c43879886f"
)
axk_verify_fatfs_file(
  "source/ff.h"
  "ecc3528886f6cd4cc2c278c8c526cf53303f4f4c49824dcd6168ab33395e1ebd76d8f7ce3135ecc28e92c2766f5f14cbbaf621767f6868569d23646a6dabceeb"
)
axk_verify_fatfs_file(
  "source/diskio.h"
  "2e3c6b6c4236a295472168a7d00446a824a34035a333255fd4f0fcd80fe8d3ec3a2bd288710e01ab9043165b94d4e724d2d1b62cb034fac1750dbf00d3c6b941"
)
axk_verify_fatfs_file(
  "LICENSE.txt"
  "ee9f69b3b12e3fae74d7831e91a4ef2a13eea23e3be891a916ff0d2ad5023f0629b9549dd030755be796c0617634c9f9e042951d82f8c2c4dd9f86f95f3976ad"
)

set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/fatfs-r0.16")
file(REMOVE_RECURSE "${SOURCE_PATH}")
file(MAKE_DIRECTORY "${SOURCE_PATH}")
file(COPY "${FATFS_SUBMODULE_ROOT}/source/ff.c"
          "${FATFS_SUBMODULE_ROOT}/source/ff.h"
          "${FATFS_SUBMODULE_ROOT}/source/diskio.h"
          "${FATFS_SUBMODULE_ROOT}/LICENSE.txt"
          "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
          "${CMAKE_CURRENT_LIST_DIR}/ffconf.h"
     DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/FatFs")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${SOURCE_PATH}/LICENSE.txt"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
     RENAME copyright)
