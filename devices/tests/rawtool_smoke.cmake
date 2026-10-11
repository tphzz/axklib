cmake_minimum_required(VERSION 3.21)

if(NOT EXISTS "${RAWDISK_TOOL}" OR NOT RAWDISK_TEST_DIRECTORY)
    message(FATAL_ERROR "rawtool and an isolated test directory are required")
endif()

file(MAKE_DIRECTORY "${RAWDISK_TEST_DIRECTORY}")
set(source "${RAWDISK_TEST_DIRECTORY}/source & image.img")
set(backup "${RAWDISK_TEST_DIRECTORY}/backup.img")
string(REPEAT "0123456789ABCDEF" 131072 data)
file(WRITE "${source}" "${data}")
file(SHA256 "${source}" before)

# Encode every path byte, including spaces and query separators, for a locator.
file(TO_CMAKE_PATH "${source}" source)
string(HEX "${source}" path_hex)
string(REGEX REPLACE "(..)" "%\\1" encoded_path "${path_hex}")
set(locator "image?image.path=${encoded_path}&image.sector_size=512")

foreach(command IN ITEMS info dump backup)
    set(arguments --no-prompt "${command}" "${locator}")
    if(command STREQUAL "backup")
        list(APPEND arguments "${backup}")
    endif()
    execute_process(COMMAND "${RAWDISK_TOOL}" ${arguments}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 20)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "rawtool ${command} failed (${result}):\n${output}\n${error}")
    endif()
endforeach()

file(SHA256 "${source}" after)
file(SHA256 "${backup}" copied)
if(NOT before STREQUAL after OR NOT before STREQUAL copied OR EXISTS "${backup}.partial")
    message(FATAL_ERROR "Read-only CLI backup changed its source or did not produce an exact completed copy")
endif()
file(REMOVE_RECURSE "${RAWDISK_TEST_DIRECTORY}")
message(STATUS "Read-only CLI info, dump and exact backup passed")
