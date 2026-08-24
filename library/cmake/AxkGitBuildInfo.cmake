include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/AxkVersionRef.cmake")

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
  set(github_ref_type "")
  set(github_ref_name "")
  if(DEFINED ENV{GITHUB_REF_TYPE})
    set(github_ref_type "$ENV{GITHUB_REF_TYPE}")
  endif()
  if(DEFINED ENV{GITHUB_REF_NAME})
    set(github_ref_name "$ENV{GITHUB_REF_NAME}")
  endif()

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
    if(github_ref_type STREQUAL "tag" AND NOT github_ref_name STREQUAL "")
      set(ref_name "${github_ref_name}")
      set(git_tag "${github_ref_name}")
      set(is_tagged_release ON)
    elseif(github_ref_type STREQUAL "branch" AND NOT github_ref_name STREQUAL "")
      set(ref_name "${github_ref_name}")
      set(git_branch "${github_ref_name}")
    elseif(git_branch_result EQUAL 0 AND NOT git_branch_output STREQUAL "" AND
       NOT git_branch_output STREQUAL "HEAD")
      set(ref_name "${git_branch_output}")
      set(git_branch "${git_branch_output}")
    else()
      axk_find_exact_semver_tag("${git_executable}" "${source_directory}")
      if(NOT AXK_EXACT_SEMVER_TAG STREQUAL "")
        set(ref_name "${AXK_EXACT_SEMVER_TAG}")
        set(git_tag "${AXK_EXACT_SEMVER_TAG}")
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
  axk_parse_version_branch("${ref_name}")
  if(NOT is_tagged_release AND AXK_VERSION_BRANCH_VALID)
    set(identity_ref "${AXK_VERSION_BRANCH_CORE}-pre")
  else()
    set(identity_ref "${sanitized_ref}")
  endif()
  if(is_dirty)
    set(source_identity "${identity_ref}-dirty-${git_sha_short}")
  else()
    set(source_identity "${identity_ref}-${git_sha_short}")
  endif()
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
  set(AXK_GIT_TAG "${git_tag}" PARENT_SCOPE)
  set(AXK_GIT_BRANCH "${git_branch}" PARENT_SCOPE)
  set(AXK_GIT_SHA_SHORT "${git_sha_short}" PARENT_SCOPE)
  set(AXK_GIT_DIRTY "${is_dirty}" PARENT_SCOPE)
  set(AXK_SOURCE_IDENTITY "${source_identity}" PARENT_SCOPE)
  set(AXK_PACKAGE_BASENAME "${package_basename}" PARENT_SCOPE)
  set(AXK_IS_TAGGED_RELEASE "${is_tagged_release}" PARENT_SCOPE)
endfunction()
