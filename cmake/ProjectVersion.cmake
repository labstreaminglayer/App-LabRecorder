# ProjectVersion.cmake
# Derive the project version from the most recent git tag.
#
# Tags may be `1.18.0`, `v1.18.0`, or prereleases such as `v1.18.0b1`. This module
# turns the nearest such tag into:
#   - a strict MAJOR.MINOR.PATCH string suitable for project(... VERSION ...)
#   - the full tag (sans leading `v`), preserving any pre-release suffix, for
#     packaging metadata.
#
# It must be included and called BEFORE project(), since project() needs the
# numeric version. When git or a matching tag is unavailable (release tarballs,
# shallow clones without tags, FetchContent on an exported tree) it falls back
# to the supplied literal so the source tree always builds.
#
# Usage:
#   include(cmake/ProjectVersion.cmake)
#   labrecorder_version_from_git(LABRECORDER_VERSION LABRECORDER_VERSION_FULL FALLBACK "1.18.0")
#   project(LabRecorder VERSION ${LABRECORDER_VERSION} ...)
#
# A version can be forced (e.g. from CI) by passing -DLABRECORDER_VERSION_OVERRIDE=1.18.0b1.

function(labrecorder_version_from_git out_numeric out_full)
    cmake_parse_arguments(ARG "" "FALLBACK" "" ${ARGN})

    set(_ver "")
    if(LABRECORDER_VERSION_OVERRIDE)
        string(REGEX REPLACE "^v" "" _ver "${LABRECORDER_VERSION_OVERRIDE}")
        message(STATUS "LabRecorder: using LABRECORDER_VERSION_OVERRIDE=${_ver}")
    else()
        # Do not accidentally use a parent repository when building an extracted archive.
        find_package(Git QUIET)
        if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "v[0-9]*" --match "[0-9]*"
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE _tag
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _res
            )
            if(_res EQUAL 0 AND _tag)
                string(REGEX REPLACE "^v" "" _ver "${_tag}")
            endif()
        endif()
    endif()

    if(NOT _ver)
        set(_ver "${ARG_FALLBACK}")
        message(STATUS "LabRecorder: no git tag found; using fallback version ${_ver}")
    endif()

    # Strict numeric MAJOR.MINOR.PATCH for project(VERSION); ignore any suffix.
    if(_ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(${out_numeric} "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}" PARENT_SCOPE)
        set(${out_full} "${_ver}" PARENT_SCOPE)
        message(STATUS "LabRecorder version: ${_ver}")
    else()
        message(WARNING "LabRecorder: version '${_ver}' is not MAJOR.MINOR.PATCH; using ${ARG_FALLBACK}")
        set(${out_numeric} "${ARG_FALLBACK}" PARENT_SCOPE)
        set(${out_full} "${ARG_FALLBACK}" PARENT_SCOPE)
    endif()
endfunction()
