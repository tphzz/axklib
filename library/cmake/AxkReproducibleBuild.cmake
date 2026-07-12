include_guard(GLOBAL)

function(axk_enable_reproducible_build_paths)
  get_property(_axk_enabled GLOBAL PROPERTY AXK_REPRODUCIBLE_BUILD_PATHS_ENABLED)
  if(_axk_enabled)
    return()
  endif()

  set(_axk_source_root "$ENV{AXK_PATH_REMAP_FROM}")
  if(NOT _axk_source_root)
    return()
  endif()

  if(MSVC)
    add_compile_options("/pathmap:${_axk_source_root}=axklib")
  else()
    add_compile_options("-ffile-prefix-map=${_axk_source_root}=/usr/src/axklib")
  endif()

  set_property(GLOBAL PROPERTY AXK_REPRODUCIBLE_BUILD_PATHS_ENABLED TRUE)
endfunction()

function(axk_remove_target_pdb_path target)
  if(MSVC)
    target_link_options(${target} PRIVATE "/PDBALTPATH:${target}.pdb")
  endif()
endfunction()
