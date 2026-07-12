execute_process(
  COMMAND "${AXK_NM}" -D --defined-only "${AXK_LIBRARY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "nm failed for ${AXK_LIBRARY}")
endif()
string(REGEX MATCHALL "[ \t][A-TV-Z][ \t]+axk_[A-Za-z0-9_]+(@@AXKLIB_C_[0-9.]+)?" matches "${output}")
set(actual "")
foreach(match IN LISTS matches)
  string(REGEX REPLACE ".*[ \t](axk_[A-Za-z0-9_]+).*" "\\1" symbol "${match}")
  list(APPEND actual "${symbol}")
endforeach()
list(SORT actual)
file(STRINGS "${AXK_BASELINE}" expected)
list(SORT expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "C ABI symbols differ from baseline\nexpected=${expected}\nactual=${actual}")
endif()
