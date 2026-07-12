if(NOT DEFINED AXK_GENERATOR OR NOT DEFINED AXK_TEST_ROOT)
  message(FATAL_ERROR "AXK_GENERATOR and AXK_TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${AXK_TEST_ROOT}")
file(MAKE_DIRECTORY "${AXK_TEST_ROOT}")

function(run_generator manifest output result_variable)
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}"
      "-DAXK_ABI_MANIFEST=${manifest}"
      "-DAXK_ABI_OUTPUT_DIR=${output}"
      -P "${AXK_GENERATOR}"
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_QUIET
  )
  set(${result_variable} "${result}" PARENT_SCOPE)
endfunction()

set(valid "${AXK_TEST_ROOT}/valid.abi")
file(WRITE "${valid}"
     "abi_node AXKLIB_C_1.0\nlibrary axklib_c\nsymbol axk_alpha 1.0 active\n"
     "symbol axk_beta 1.0 deprecated\n")
run_generator("${valid}" "${AXK_TEST_ROOT}/generated" result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "valid ABI manifest was rejected")
endif()

file(READ "${AXK_TEST_ROOT}/generated/axklib_c.symbols" symbols)
file(READ "${AXK_TEST_ROOT}/generated/axklib_c.map" map)
file(READ "${AXK_TEST_ROOT}/generated/axklib_c.exports" exports)
file(READ "${AXK_TEST_ROOT}/generated/axklib_c.def" def)
if(NOT symbols STREQUAL "axk_alpha\naxk_beta\n")
  message(FATAL_ERROR "unexpected normalized symbol output")
endif()
if(NOT map STREQUAL
   "AXKLIB_C_1.0 {\n  global:\n    axk_alpha;\n    axk_beta;\n  local: *;\n};\n")
  message(FATAL_ERROR "unexpected ELF version script")
endif()
if(NOT exports STREQUAL "_axk_alpha\n_axk_beta\n")
  message(FATAL_ERROR "unexpected Mach-O export list")
endif()
if(NOT def STREQUAL "LIBRARY axklib_c\nEXPORTS\n  axk_alpha\n  axk_beta\n")
  message(FATAL_ERROR "unexpected PE definition file")
endif()

foreach(case_name IN ITEMS duplicate invalid unsorted unsupported)
  set(manifest "${AXK_TEST_ROOT}/${case_name}.abi")
  if(case_name STREQUAL "duplicate")
    file(WRITE "${manifest}"
         "abi_node AXKLIB_C_1.0\nlibrary axklib_c\nsymbol axk_alpha 1.0 active\n"
         "symbol axk_alpha 1.0 active\n")
  elseif(case_name STREQUAL "invalid")
    file(WRITE "${manifest}"
         "abi_node AXKLIB_C_1.0\nlibrary axklib_c\nsymbol invalid-name 1.0 active\n")
  elseif(case_name STREQUAL "unsorted")
    file(WRITE "${manifest}"
         "abi_node AXKLIB_C_1.0\nlibrary axklib_c\nsymbol axk_beta 1.0 active\n"
         "symbol axk_alpha 1.0 active\n")
  else()
    file(WRITE "${manifest}"
         "abi_node AXKLIB_C_1.0\nlibrary axklib_c\nsymbol axk_alpha 2.0 active\n")
  endif()
  run_generator("${manifest}" "${AXK_TEST_ROOT}/${case_name}-out" result)
  if(result EQUAL 0)
    message(FATAL_ERROR "${case_name} ABI manifest was accepted")
  endif()
endforeach()
