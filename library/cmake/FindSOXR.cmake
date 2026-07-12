find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_SOXR QUIET soxr)
endif()

find_path(
  SOXR_INCLUDE_DIR
  NAMES soxr.h
  HINTS ${PC_SOXR_INCLUDE_DIRS}
)
find_library(
  SOXR_LIBRARY
  NAMES soxr
  HINTS ${PC_SOXR_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  SOXR
  REQUIRED_VARS SOXR_LIBRARY SOXR_INCLUDE_DIR
)

if(SOXR_FOUND AND NOT TARGET SOXR::soxr)
  add_library(SOXR::soxr UNKNOWN IMPORTED)
  set_target_properties(
    SOXR::soxr
    PROPERTIES
      IMPORTED_LOCATION "${SOXR_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${SOXR_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(SOXR_INCLUDE_DIR SOXR_LIBRARY)