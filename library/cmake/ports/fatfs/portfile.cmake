vcpkg_download_distfile(
  ARCHIVE
  URLS "https://elm-chan.org/fsw/ff/arc/ff16.zip"
  FILENAME "fatfs-0.16.zip"
  SHA512 ece81294612cedb78035df164b830771f95833a00b630f1ff97cfa7502237588856fac1638f0cdf5aa12b82d9294e7fb6bff8931cbd565861dee4f12a62b82d3
)

vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" NO_REMOVE_ONE_LEVEL)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
     "${CMAKE_CURRENT_LIST_DIR}/ffconf.h"
     DESTINATION "${SOURCE_PATH}/source")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}/source")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/FatFs")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${SOURCE_PATH}/LICENSE.txt"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
     RENAME copyright)
