if(AXK_SYSTEM_NAME STREQUAL "Windows")
  if(AXK_DUMPBIN STREQUAL "")
    set(AXK_DUMPBIN dumpbin)
  endif()
  execute_process(
    COMMAND "${AXK_DUMPBIN}" /nologo /exports "${AXK_LIBRARY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
  )
elseif(AXK_SYSTEM_NAME STREQUAL "Darwin")
  execute_process(
    COMMAND "${AXK_NM}" -g "${AXK_LIBRARY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
  )
else()
  execute_process(
    COMMAND "${AXK_NM}" -D --defined-only "${AXK_LIBRARY}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
  )
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "symbol inspection failed for ${AXK_LIBRARY}")
endif()
if(AXK_SYSTEM_NAME STREQUAL "Windows")
  string(REGEX MATCHALL "[ \t]axk_[A-Za-z0-9_]+" matches "${output}")
else()
  string(REGEX MATCHALL "[ \t][A-TV-Z][ \t]+_?axk_[A-Za-z0-9_]+(@@AXKLIB_C_[0-9.]+)?" matches "${output}")
endif()
set(actual "")
foreach(match IN LISTS matches)
  string(REGEX REPLACE ".*[ \t]_?(axk_[A-Za-z0-9_]+).*" "\\1" symbol "${match}")
  list(APPEND actual "${symbol}")
endforeach()
list(SORT actual)
list(REMOVE_DUPLICATES actual)
file(STRINGS "${AXK_BASELINE}" expected)
list(SORT expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "C ABI symbols differ from baseline\nexpected=${expected}\nactual=${actual}")
endif()
