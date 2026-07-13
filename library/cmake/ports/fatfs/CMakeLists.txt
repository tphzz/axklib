cmake_minimum_required(VERSION 3.20)
project(FatFs LANGUAGES C)

add_library(FatFs STATIC ff.c)
add_library(FatFs::FatFs ALIAS FatFs)
target_include_directories(
  FatFs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/fatfs>
)
set_target_properties(FatFs PROPERTIES EXPORT_NAME FatFs POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
install(TARGETS FatFs EXPORT FatFsTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES ff.h diskio.h ffconf.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fatfs")
install(EXPORT FatFsTargets FILE FatFsConfig.cmake NAMESPACE FatFs::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/FatFs")
