include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/AxkVersionRef.cmake")

function(axk_derive_project_version source_directory)
  set(semantic_version "0.0.0")
  set(project_version "0.0.0")
  set(version_major 0)
  set(version_minor 0)
  set(version_patch 0)
  set(version_prerelease "")
  set(version_build_metadata "")
  set(release_tag "")
  set(is_prerelease OFF)
  set(git_executable "")
  set(github_ref_type "")
  if(DEFINED ENV{GITHUB_REF_TYPE})
    set(github_ref_type "$ENV{GITHUB_REF_TYPE}")
  endif()

  if(DEFINED AXK_GIT_EXECUTABLE)
    set(git_executable "${AXK_GIT_EXECUTABLE}")
  else()
    find_package(Git QUIET)
    if(Git_FOUND)
      set(git_executable "${GIT_EXECUTABLE}")
    endif()
  endif()

  set(branch_name "")
  if(github_ref_type STREQUAL "branch" AND DEFINED ENV{GITHUB_REF_NAME} AND
     NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
    set(branch_name "$ENV{GITHUB_REF_NAME}")
  endif()
  if(NOT git_executable STREQUAL "")
    execute_process(
      COMMAND "${git_executable}" rev-parse --abbrev-ref HEAD
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE branch_result
      OUTPUT_VARIABLE branch_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(branch_name STREQUAL "" AND branch_result EQUAL 0 AND
       NOT branch_output STREQUAL "" AND NOT branch_output STREQUAL "HEAD")
      set(branch_name "${branch_output}")
    endif()
  endif()

  set(selected_tag "")
  if(github_ref_type STREQUAL "tag")
    if(git_executable STREQUAL "")
      message(FATAL_ERROR "GitHub tag builds require Git")
    endif()
    if(NOT DEFINED ENV{GITHUB_REF_NAME} OR "$ENV{GITHUB_REF_NAME}" STREQUAL "")
      message(FATAL_ERROR "GitHub tag builds require GITHUB_REF_NAME")
    endif()
    set(candidate_tag "$ENV{GITHUB_REF_NAME}")
    axk_parse_semver_ref("${candidate_tag}")
    if(NOT AXK_SEMVER_REF_VALID)
      message(FATAL_ERROR "GitHub ref '${candidate_tag}' is not a valid semantic version tag")
    endif()
    execute_process(
      COMMAND "${git_executable}" rev-parse --verify "refs/tags/${candidate_tag}^{commit}"
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE tag_commit_result
      OUTPUT_VARIABLE tag_commit
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
      COMMAND "${git_executable}" rev-parse --verify HEAD
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE head_commit_result
      OUTPUT_VARIABLE head_commit
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT tag_commit_result EQUAL 0 OR NOT head_commit_result EQUAL 0 OR
       NOT tag_commit STREQUAL head_commit)
      message(FATAL_ERROR "GitHub release tag '${candidate_tag}' does not identify HEAD")
    endif()
    axk_find_exact_semver_tag("${git_executable}" "${source_directory}")
    if(NOT AXK_EXACT_SEMVER_TAG STREQUAL "${candidate_tag}")
      message(FATAL_ERROR
        "GitHub release tag '${candidate_tag}' is not the exact semantic version tag at HEAD")
    endif()
    set(selected_tag "${candidate_tag}")
  elseif(branch_name STREQUAL "" AND NOT git_executable STREQUAL "")
    axk_find_exact_semver_tag("${git_executable}" "${source_directory}")
    set(selected_tag "${AXK_EXACT_SEMVER_TAG}")
  endif()

  if(NOT selected_tag STREQUAL "")
    axk_parse_semver_ref("${selected_tag}")
    set(semantic_version "${AXK_SEMVER_NORMALIZED}")
    set(version_major "${AXK_SEMVER_MAJOR}")
    set(version_minor "${AXK_SEMVER_MINOR}")
    set(version_patch "${AXK_SEMVER_PATCH}")
    set(version_prerelease "${AXK_SEMVER_PRERELEASE}")
    set(version_build_metadata "${AXK_SEMVER_BUILD_METADATA}")
    set(project_version "${version_major}.${version_minor}.${version_patch}")
    set(release_tag "${selected_tag}")
    if(NOT version_prerelease STREQUAL "")
      set(is_prerelease ON)
    endif()
  elseif(NOT branch_name STREQUAL "")
    axk_parse_version_branch("${branch_name}")
    if(AXK_VERSION_BRANCH_VALID)
      set(semantic_version "${AXK_VERSION_BRANCH_CORE}-pre")
      set(project_version "${AXK_VERSION_BRANCH_CORE}")
      set(version_major "${AXK_VERSION_BRANCH_MAJOR}")
      set(version_minor "${AXK_VERSION_BRANCH_MINOR}")
      set(version_patch "${AXK_VERSION_BRANCH_PATCH}")
      set(version_prerelease pre)
      set(is_prerelease ON)
    endif()
  endif()

  set(AXK_SEMANTIC_VERSION "${semantic_version}" PARENT_SCOPE)
  set(AXK_PROJECT_VERSION "${project_version}" PARENT_SCOPE)
  set(AXK_VERSION_MAJOR "${version_major}" PARENT_SCOPE)
  set(AXK_VERSION_MINOR "${version_minor}" PARENT_SCOPE)
  set(AXK_VERSION_PATCH "${version_patch}" PARENT_SCOPE)
  set(AXK_VERSION_PRERELEASE "${version_prerelease}" PARENT_SCOPE)
  set(AXK_VERSION_BUILD_METADATA "${version_build_metadata}" PARENT_SCOPE)
  set(AXK_RELEASE_TAG "${release_tag}" PARENT_SCOPE)
  set(AXK_VERSION_IS_PRERELEASE "${is_prerelease}" PARENT_SCOPE)
endfunction()

function(axk_write_project_version_metadata output_path)
  if(AXK_RELEASE_TAG STREQUAL "")
    set(is_release_json false)
  else()
    set(is_release_json true)
  endif()
  if(AXK_VERSION_IS_PRERELEASE)
    set(is_prerelease_json true)
  else()
    set(is_prerelease_json false)
  endif()

  set(metadata "{\n")
  string(APPEND metadata "  \"schema_version\": 1,\n")
  string(APPEND metadata "  \"semantic_version\": \"${AXK_SEMANTIC_VERSION}\",\n")
  string(APPEND metadata "  \"project_version\": \"${AXK_PROJECT_VERSION}\",\n")
  string(APPEND metadata "  \"major\": ${AXK_VERSION_MAJOR},\n")
  string(APPEND metadata "  \"minor\": ${AXK_VERSION_MINOR},\n")
  string(APPEND metadata "  \"patch\": ${AXK_VERSION_PATCH},\n")
  string(APPEND metadata "  \"release_tag\": \"${AXK_RELEASE_TAG}\",\n")
  string(APPEND metadata "  \"is_release\": ${is_release_json},\n")
  string(APPEND metadata "  \"is_prerelease\": ${is_prerelease_json}\n")
  string(APPEND metadata "}\n")

  get_filename_component(output_directory "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  set(temporary_path "${output_path}.tmp")
  file(WRITE "${temporary_path}" "${metadata}")
  file(COPY_FILE "${temporary_path}" "${output_path}" ONLY_IF_DIFFERENT)
  file(REMOVE "${temporary_path}")
endfunction()
