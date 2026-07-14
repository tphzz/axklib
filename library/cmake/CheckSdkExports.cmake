if(NOT DEFINED AXK_SHARED_LIBRARY OR NOT DEFINED AXK_NM)
  message(FATAL_ERROR "shared SDK export check requires AXK_SHARED_LIBRARY and AXK_NM")
endif()

if(WIN32)
  execute_process(
    COMMAND dumpbin /exports "${AXK_SHARED_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error
  )
elseif(APPLE)
  execute_process(
    COMMAND "${AXK_NM}" -gU "${AXK_SHARED_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error
  )
else()
  execute_process(
    COMMAND "${AXK_NM}" -D --defined-only -C "${AXK_SHARED_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error
  )
endif()
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "nm failed for ${AXK_SHARED_LIBRARY}: ${nm_error}")
endif()

if(WIN32)
  if(NOT symbols MATCHES "sdk_version@axk")
    message(FATAL_ERROR "shared SDK does not export the C++ facade")
  endif()
  if(symbols MATCHES "(FLAC__|[?@_]sf_|soxr_|vorbis_|ogg_|opus_|axk_c_|c_api)")
    message(FATAL_ERROR "shared SDK exports a private dependency or retired C symbol")
  endif()
elseif(APPLE)
  if(symbols MATCHES "(_FLAC__|_sf_|_soxr_|_vorbis_|_ogg_|_opus_)")
    message(FATAL_ERROR "shared SDK exports a private codec/resampler symbol")
  endif()
else()
  string(REPLACE "\n" ";" symbol_lines "${symbols}")
  foreach(line IN LISTS symbol_lines)
    if(line STREQUAL "")
      continue()
    endif()
    if(line MATCHES " axk::")
      if(line MATCHES "::impl")
        message(FATAL_ERROR "shared SDK exports private implementation RTTI: ${line}")
      endif()
      if(NOT line MATCHES
         "axk::(build_plan|package_import_plan|portable_package|sdk_version|transaction|render_error|progress_sink|operation_context|image|snapshot)")
        message(FATAL_ERROR "shared SDK exports a non-facade axklib symbol: ${line}")
      endif()
    elseif(line MATCHES " [VvWwu] .*std::")
      # Toolchain-emitted weak C++ runtime template and RTTI symbols are not
      # axklib API and do not name any private engine or dependency facility.
    else()
      message(FATAL_ERROR "unexpected shared SDK export: ${line}")
    endif()
  endforeach()
endif()
