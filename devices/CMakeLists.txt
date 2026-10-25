cmake_minimum_required(VERSION 3.21)
project(rawdisk VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RAWDISK_BUILD_TOOLS "Build rawtool and the examples" ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(RAWDISK_BUILD_TESTS "Build tests" ON)
else()
    option(RAWDISK_BUILD_TESTS "Build tests" ${BUILD_TESTING})
endif()
option(RAWDISK_WERROR "Treat warnings as errors" ON)

# Compiler and Windows SDK policy for every target of this project.
function(rawdisk_target_options target)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8
            $<$<BOOL:${RAWDISK_WERROR}>:/WX>)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow
            -Wconversion -Wsign-conversion $<$<BOOL:${RAWDISK_WERROR}>:-Werror>)
    endif()
endfunction()

find_package(Threads REQUIRED)
add_subdirectory(cmake/expat)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    foreach(component sdk cli server)
        install(FILES vendor/expat/COPYING DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/expat"
            COMPONENT ${component})
    endforeach()
endif()

add_library(rawdisk
    # core
    src/errors.cpp
    src/block_device.cpp
    src/authorization_session.cpp
    src/ownership.cpp
    src/remote.cpp
    src/probe.cpp
    src/probe_format.cpp
    src/probe_storage.cpp
    # layer 1: typed API on top of BlockDevice
    src/slice.cpp
    src/volume.cpp
    src/scsi2sd_common.cpp
    src/scsi2sd_v6.cpp
    src/scsi2sd_v6_primary.cpp
    src/scsi2sd_xml.cpp
    # layer 2/3: open flows, modes, locators
    src/open_flow.cpp
    src/open_blocks.cpp
    src/open_modes.cpp
    src/open_locator.cpp)
target_include_directories(rawdisk PUBLIC include PRIVATE src)
target_link_libraries(rawdisk PUBLIC Threads::Threads PRIVATE rawdisk_expat)
target_compile_features(rawdisk PUBLIC cxx_std_23)
set_target_properties(rawdisk PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
add_library(axklib::devices ALIAS rawdisk)
rawdisk_target_options(rawdisk)

if(WIN32)
    target_sources(rawdisk PRIVATE src/win32_disk.cpp src/win32_identity.cpp src/win32_elevation.cpp src/win32_pipe.cpp)
    target_link_libraries(rawdisk PRIVATE advapi32 shell32 ole32 bcrypt setupapi cfgmgr32 uuid)

    # The elevated helper. Must be deployed next to the app's .exe (or configure
    # ElevationOptions::helper_path). WIN32 = GUI subsystem: no console window.
    add_executable(axklib_helper WIN32 src/win32_helper_main.cpp)
    target_link_libraries(axklib_helper PRIVATE rawdisk shell32)
    rawdisk_target_options(axklib_helper)
    if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        install(TARGETS axklib_helper RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT server)
        install(TARGETS axklib_helper RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT sdk)
    endif()
    if(MSVC)
        # Resolve the helper's own DLL imports from System32 only (DLL-planting hardening
        # for an elevated process; Windows 10 1607+, ignored by older systems).
        target_link_options(axklib_helper PRIVATE /DEPENDENTLOADFLAG:0x800)
    endif()
elseif(APPLE)
    target_sources(rawdisk PRIVATE src/posix_disk.cpp src/posix_authorization.cpp src/macos_authorization.cpp src/enumerate_macos.cpp src/macos_claim.cpp)
    target_link_libraries(rawdisk PRIVATE
        "-framework CoreFoundation" "-framework DiskArbitration" "-framework IOKit" "-framework Security")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RAWDISK_DBUS REQUIRED IMPORTED_TARGET dbus-1>=1.10)
    target_sources(rawdisk PRIVATE src/posix_disk.cpp src/posix_authorization.cpp src/enumerate_linux.cpp src/linux_authorization.cpp)
    target_link_libraries(rawdisk PRIVATE PkgConfig::RAWDISK_DBUS)
else()
    message(FATAL_ERROR "rawdisk supports Windows, macOS and Linux")
endif()

if(RAWDISK_BUILD_TOOLS)
    foreach(tool rawtool typed_api)
        add_executable(${tool} examples/${tool}.cpp)
        target_link_libraries(${tool} PRIVATE rawdisk)
        rawdisk_target_options(${tool})
        if(WIN32)
            add_dependencies(${tool} axklib_helper) # same output directory, so it is found at runtime
        endif()
    endforeach()
    target_sources(rawtool PRIVATE examples/rawtool_probe.cpp)
endif()

if(RAWDISK_BUILD_TESTS)
    enable_testing()
    foreach(t rawdisk_tests authorization_session_tests layout_tests v6_primary_tests open_tests cancellation_tests probe_tests ownership_tests
            win32_identity_tests)
        if(t STREQUAL "rawdisk_tests")
            add_executable(${t} tests/tests.cpp)
        else()
            add_executable(${t} tests/${t}.cpp)
        endif()
        target_include_directories(${t} PRIVATE src tests)
        target_link_libraries(${t} PRIVATE rawdisk)
        rawdisk_target_options(${t})
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES TIMEOUT 120)
    endforeach()

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(linux_authorization_tests tests/linux_authorization_tests.cpp)
        target_include_directories(linux_authorization_tests PRIVATE src tests)
        target_link_libraries(linux_authorization_tests PRIVATE rawdisk PkgConfig::RAWDISK_DBUS)
        rawdisk_target_options(linux_authorization_tests)
        add_test(NAME linux_authorization_tests COMMAND linux_authorization_tests)
        set_tests_properties(linux_authorization_tests PROPERTIES TIMEOUT 30)
        add_executable(linux_authorization_session_tests tests/linux_authorization_session_tests.cpp)
        target_include_directories(linux_authorization_session_tests PRIVATE src tests)
        target_link_libraries(linux_authorization_session_tests PRIVATE rawdisk PkgConfig::RAWDISK_DBUS)
        rawdisk_target_options(linux_authorization_session_tests)
        add_test(NAME linux_authorization_session_tests COMMAND linux_authorization_session_tests)
        set_tests_properties(linux_authorization_session_tests PROPERTIES TIMEOUT 30)
        # Test-only syscall fault injection; never enabled in the library or CLI.
        target_compile_definitions(probe_tests PRIVATE RAWDISK_TEST_WRAP_FSYNC=1)
        target_link_options(probe_tests PRIVATE -Wl,--wrap=fsync)
    endif()

    if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(posix_authorization_tests tests/posix_authorization_tests.cpp)
        target_include_directories(posix_authorization_tests PRIVATE src tests)
        target_link_libraries(posix_authorization_tests PRIVATE rawdisk)
        rawdisk_target_options(posix_authorization_tests)
        add_test(NAME posix_authorization_tests COMMAND posix_authorization_tests)
        set_tests_properties(posix_authorization_tests PROPERTIES TIMEOUT 30)
        add_executable(authorization_transfer_tests tests/authorization_transfer_tests.cpp)
        target_include_directories(authorization_transfer_tests PRIVATE src tests)
        target_link_libraries(authorization_transfer_tests PRIVATE rawdisk)
        rawdisk_target_options(authorization_transfer_tests)
        add_test(NAME authorization_transfer_tests COMMAND authorization_transfer_tests)
        set_tests_properties(authorization_transfer_tests PROPERTIES TIMEOUT 30)
    endif()

    if(APPLE)
        add_executable(macos_authorization_tests tests/macos_authorization_tests.cpp)
        target_include_directories(macos_authorization_tests PRIVATE src tests)
        target_link_libraries(macos_authorization_tests PRIVATE rawdisk "-framework Security")
        rawdisk_target_options(macos_authorization_tests)
        add_test(NAME macos_authorization_tests COMMAND macos_authorization_tests)
        set_tests_properties(macos_authorization_tests PROPERTIES TIMEOUT 30)
        add_executable(macos_claim_tests tests/macos_claim_tests.cpp)
        target_include_directories(macos_claim_tests PRIVATE src tests)
        target_link_libraries(macos_claim_tests PRIVATE rawdisk "-framework CoreFoundation" "-framework DiskArbitration")
        rawdisk_target_options(macos_claim_tests)
        add_test(NAME macos_claim_tests COMMAND macos_claim_tests)
        set_tests_properties(macos_claim_tests PROPERTIES TIMEOUT 30)
    endif()

    file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/rawdisk/*.hpp")
    set(header_sources)
    foreach(header IN LISTS public_headers)
        string(REPLACE "/" "_" name "${header}")
        set(source "${CMAKE_CURRENT_BINARY_DIR}/header-self-containment/${name}.cpp")
        file(WRITE "${source}" "#include <${header}>\n")
        list(APPEND header_sources "${source}")
    endforeach()
    add_library(rawdisk_header_self_containment OBJECT ${header_sources})
    target_link_libraries(rawdisk_header_self_containment PRIVATE rawdisk)
    rawdisk_target_options(rawdisk_header_self_containment)

    if(RAWDISK_BUILD_TOOLS)
        add_executable(probe_fixture tests/probe_fixture.cpp)
        target_link_libraries(probe_fixture PRIVATE rawdisk)
        rawdisk_target_options(probe_fixture)
        add_test(NAME rawtool_write_smoke COMMAND ${CMAKE_COMMAND}
            "-DRAWDISK_TOOL=$<TARGET_FILE:rawtool>"
            "-DPROBE_FIXTURE=$<TARGET_FILE:probe_fixture>"
            "-DRAWDISK_TEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/rawtool-write-smoke"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rawtool_write_smoke.cmake")
        set_tests_properties(rawtool_write_smoke PROPERTIES TIMEOUT 120)
        add_test(NAME rawtool_smoke COMMAND ${CMAKE_COMMAND}
            "-DRAWDISK_TOOL=$<TARGET_FILE:rawtool>"
            "-DRAWDISK_TEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/rawtool-smoke"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rawtool_smoke.cmake")
        set_tests_properties(rawtool_smoke PROPERTIES TIMEOUT 120)
    endif()

    if(WIN32)
        add_executable(win32_helper_tests tests/win32_helper_tests.cpp)
        target_include_directories(win32_helper_tests PRIVATE src tests)
        target_link_libraries(win32_helper_tests PRIVATE rawdisk)
        rawdisk_target_options(win32_helper_tests)
        add_dependencies(win32_helper_tests axklib_helper)
        add_test(NAME win32_helper_tests COMMAND win32_helper_tests "$<TARGET_FILE:axklib_helper>")
        set_tests_properties(win32_helper_tests PROPERTIES TIMEOUT 120 LABELS "windows-helper")
    endif()
endif()
