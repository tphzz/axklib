include_guard(GLOBAL)

function(axk_parse_semver_ref ref)
  set(valid OFF)
  set(major "")
  set(minor "")
  set(patch "")
  set(prerelease "")
  set(build_metadata "")
  set(normalized "")

  if("${ref}" MATCHES
     "^v?(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)(-([0-9A-Za-z.-]+))?([+]([0-9A-Za-z.-]+))?$")
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

    if(valid)
      set(normalized "${major}.${minor}.${patch}")
      if(NOT prerelease STREQUAL "")
        string(APPEND normalized "-${prerelease}")
      endif()
      if(NOT build_metadata STREQUAL "")
        string(APPEND normalized "+${build_metadata}")
      endif()
    endif()
  endif()

  set(AXK_SEMVER_REF_VALID "${valid}" PARENT_SCOPE)
  set(AXK_SEMVER_NORMALIZED "${normalized}" PARENT_SCOPE)
  set(AXK_SEMVER_MAJOR "${major}" PARENT_SCOPE)
  set(AXK_SEMVER_MINOR "${minor}" PARENT_SCOPE)
  set(AXK_SEMVER_PATCH "${patch}" PARENT_SCOPE)
  set(AXK_SEMVER_PRERELEASE "${prerelease}" PARENT_SCOPE)
  set(AXK_SEMVER_BUILD_METADATA "${build_metadata}" PARENT_SCOPE)
endfunction()

function(axk_parse_version_branch ref)
  set(candidate "${ref}")
  if(candidate MATCHES "^(release|feature|features|bugfix|bugfixes)/(.+)$")
    set(candidate "${CMAKE_MATCH_2}")
  endif()

  axk_parse_semver_ref("${candidate}")
  if(AXK_SEMVER_REF_VALID AND AXK_SEMVER_PRERELEASE STREQUAL "" AND
     AXK_SEMVER_BUILD_METADATA STREQUAL "")
    set(valid ON)
    set(core "${AXK_SEMVER_MAJOR}.${AXK_SEMVER_MINOR}.${AXK_SEMVER_PATCH}")
  else()
    set(valid OFF)
    set(core "")
  endif()

  set(AXK_VERSION_BRANCH_VALID "${valid}" PARENT_SCOPE)
  set(AXK_VERSION_BRANCH_CORE "${core}" PARENT_SCOPE)
  set(AXK_VERSION_BRANCH_MAJOR "${AXK_SEMVER_MAJOR}" PARENT_SCOPE)
  set(AXK_VERSION_BRANCH_MINOR "${AXK_SEMVER_MINOR}" PARENT_SCOPE)
  set(AXK_VERSION_BRANCH_PATCH "${AXK_SEMVER_PATCH}" PARENT_SCOPE)
endfunction()

function(axk_find_exact_semver_tag git_executable source_directory)
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
      axk_parse_semver_ref("${candidate_tag}")
      if(AXK_SEMVER_REF_VALID)
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
  else()
    set(selected_tag "")
  endif()

  set(AXK_EXACT_SEMVER_TAG "${selected_tag}" PARENT_SCOPE)
endfunction()
