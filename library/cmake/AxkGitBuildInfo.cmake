include_guard(GLOBAL)

function(axk_sanitize_git_ref input output_variable)
  string(REGEX REPLACE "[^A-Za-z0-9._-]+" "-" sanitized "${input}")
  string(REGEX REPLACE "-+" "-" sanitized "${sanitized}")
  string(REGEX REPLACE "^-|-$" "" sanitized "${sanitized}")
  if(sanitized STREQUAL "")
    set(sanitized "unknown")
  endif()
  set(${output_variable} "${sanitized}" PARENT_SCOPE)
endfunction()

function(axk_derive_git_build_info source_directory product_name)
  set(git_tag "")
  set(git_branch "")
  set(git_sha_short "unknown")
  set(is_tagged_release OFF)
  set(is_dirty OFF)
  set(ref_name "local")
  set(git_executable "")

  if(DEFINED AXK_GIT_EXECUTABLE)
    set(git_executable "${AXK_GIT_EXECUTABLE}")
  else()
    find_package(Git QUIET)
    if(Git_FOUND)
      set(git_executable "${GIT_EXECUTABLE}")
    endif()
  endif()

  if(NOT git_executable STREQUAL "")
    execute_process(
      COMMAND "${git_executable}" rev-parse --short=7 HEAD
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE git_sha_result
      OUTPUT_VARIABLE git_sha_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(git_sha_result EQUAL 0 AND NOT git_sha_output STREQUAL "")
      set(git_sha_short "${git_sha_output}")
    endif()

    execute_process(
      COMMAND "${git_executable}" rev-parse --abbrev-ref HEAD
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE git_branch_result
      OUTPUT_VARIABLE git_branch_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(git_branch_result EQUAL 0 AND NOT git_branch_output STREQUAL "" AND
       NOT git_branch_output STREQUAL "HEAD")
      set(ref_name "${git_branch_output}")
      set(git_branch "${git_branch_output}")
    elseif(DEFINED ENV{GITHUB_REF_TYPE} AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag" AND
           DEFINED ENV{GITHUB_REF_NAME} AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
      set(ref_name "$ENV{GITHUB_REF_NAME}")
      set(git_tag "$ENV{GITHUB_REF_NAME}")
      set(is_tagged_release ON)
    else()
      execute_process(
        COMMAND "${git_executable}" describe --tags --exact-match HEAD
        WORKING_DIRECTORY "${source_directory}"
        RESULT_VARIABLE git_tag_result
        OUTPUT_VARIABLE git_tag_output
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      if(git_tag_result EQUAL 0 AND NOT git_tag_output STREQUAL "")
        set(ref_name "${git_tag_output}")
        set(git_tag "${git_tag_output}")
        set(is_tagged_release ON)
      else()
        set(ref_name "detached")
        set(git_branch "detached")
      endif()
    endif()

    execute_process(
      COMMAND "${git_executable}" status --porcelain --untracked-files=normal
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE git_dirty_result
      OUTPUT_VARIABLE git_dirty_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(git_dirty_result EQUAL 0 AND NOT git_dirty_output STREQUAL "")
      set(is_dirty ON)
    endif()
  endif()

  axk_sanitize_git_ref("${ref_name}" sanitized_ref)
  set(source_identity "${sanitized_ref}-${git_sha_short}")
  set(package_basename "${product_name}-${source_identity}")

  if(git_branch STREQUAL "")
    set(git_branch "${sanitized_ref}")
  else()
    axk_sanitize_git_ref("${git_branch}" git_branch)
  endif()
  if(git_tag STREQUAL "" AND is_tagged_release)
    set(git_tag "${sanitized_ref}")
  elseif(NOT git_tag STREQUAL "")
    axk_sanitize_git_ref("${git_tag}" git_tag)
  endif()
  if(is_dirty)
    string(APPEND source_identity "-mod")
    string(APPEND package_basename "-mod")
  endif()

  set(AXK_GIT_TAG "${git_tag}" PARENT_SCOPE)
  set(AXK_GIT_BRANCH "${git_branch}" PARENT_SCOPE)
  set(AXK_GIT_SHA_SHORT "${git_sha_short}" PARENT_SCOPE)
  set(AXK_GIT_DIRTY "${is_dirty}" PARENT_SCOPE)
  set(AXK_SOURCE_IDENTITY "${source_identity}" PARENT_SCOPE)
  set(AXK_PACKAGE_BASENAME "${package_basename}" PARENT_SCOPE)
  set(AXK_IS_TAGGED_RELEASE "${is_tagged_release}" PARENT_SCOPE)
endfunction()
