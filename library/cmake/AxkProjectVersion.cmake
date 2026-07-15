include_guard(GLOBAL)

function(axk_parse_semver_tag tag)
  set(valid OFF)
  set(major "")
  set(minor "")
  set(patch "")
  set(prerelease "")
  set(build_metadata "")

  if("${tag}" MATCHES
     "^v(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)(-([0-9A-Za-z.-]+))?([+]([0-9A-Za-z.-]+))?$")
    set(valid ON)
    set(major "${CMAKE_MATCH_1}")
    set(minor "${CMAKE_MATCH_2}")
    set(patch "${CMAKE_MATCH_3}")
    set(prerelease "${CMAKE_MATCH_5}")
    set(build_metadata "${CMAKE_MATCH_7}")

    foreach(identifier_group IN ITEMS prerelease build_metadata)
      if(NOT "${${identifier_group}}" STREQUAL "")
        string(REPLACE "." ";" identifiers "${${identifier_group}}")
        foreach(identifier IN LISTS identifiers)
          if(identifier STREQUAL "" OR NOT identifier MATCHES "^[0-9A-Za-z-]+$")
            set(valid OFF)
          endif()
          if(identifier_group STREQUAL "prerelease" AND
             identifier MATCHES "^[0-9]+$" AND identifier MATCHES "^0[0-9]+$")
            set(valid OFF)
          endif()
        endforeach()
      endif()
    endforeach()
  endif()

  set(AXK_SEMVER_TAG_VALID "${valid}" PARENT_SCOPE)
  set(AXK_SEMVER_MAJOR "${major}" PARENT_SCOPE)
  set(AXK_SEMVER_MINOR "${minor}" PARENT_SCOPE)
  set(AXK_SEMVER_PATCH "${patch}" PARENT_SCOPE)
  set(AXK_SEMVER_PRERELEASE "${prerelease}" PARENT_SCOPE)
  set(AXK_SEMVER_BUILD_METADATA "${build_metadata}" PARENT_SCOPE)
endfunction()

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

  set(named_branch OFF)
  if(NOT git_executable STREQUAL "")
    execute_process(
      COMMAND "${git_executable}" rev-parse --abbrev-ref HEAD
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE branch_result
      OUTPUT_VARIABLE branch_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(branch_result EQUAL 0 AND NOT branch_output STREQUAL "" AND
       NOT branch_output STREQUAL "HEAD")
      set(named_branch ON)
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
    axk_parse_semver_tag("${candidate_tag}")
    if(NOT AXK_SEMVER_TAG_VALID)
      message(FATAL_ERROR "GitHub ref '${candidate_tag}' is not a valid semantic version tag")
    endif()
    execute_process(
      COMMAND "${git_executable}" rev-parse --verify "${candidate_tag}^{commit}"
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
    set(selected_tag "${candidate_tag}")
  elseif(NOT github_ref_type STREQUAL "branch" AND NOT named_branch AND
         NOT git_executable STREQUAL "")
    execute_process(
      COMMAND "${git_executable}" tag --points-at HEAD
      WORKING_DIRECTORY "${source_directory}"
      RESULT_VARIABLE tags_result
      OUTPUT_VARIABLE tags_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(version_tags)
    if(tags_result EQUAL 0 AND NOT tags_output STREQUAL "")
      string(REPLACE "\n" ";" exact_tags "${tags_output}")
      foreach(candidate_tag IN LISTS exact_tags)
        axk_parse_semver_tag("${candidate_tag}")
        if(AXK_SEMVER_TAG_VALID)
          list(APPEND version_tags "${candidate_tag}")
        endif()
      endforeach()
    endif()
    list(LENGTH version_tags version_tag_count)
    if(version_tag_count GREATER 1)
      list(JOIN version_tags ", " version_tag_list)
      message(FATAL_ERROR "HEAD has multiple semantic version tags: ${version_tag_list}")
    elseif(version_tag_count EQUAL 1)
      list(GET version_tags 0 selected_tag)
    endif()
  endif()

  if(NOT selected_tag STREQUAL "")
    axk_parse_semver_tag("${selected_tag}")
    string(SUBSTRING "${selected_tag}" 1 -1 semantic_version)
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
