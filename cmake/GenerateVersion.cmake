if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

if(NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

find_package(Git QUIET)

set(version "0.0")
set(commit_count "0")
set(dirty_suffix "")

function(next_patch_version input_version output_variable)
    if("${input_version}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(.*)$")
        set(major "${CMAKE_MATCH_1}")
        set(minor "${CMAKE_MATCH_2}")
        math(EXPR patch "${CMAKE_MATCH_3} + 1")
        set("${output_variable}" "${major}.${minor}.${patch}" PARENT_SCOPE)
    else()
        set("${output_variable}" "${input_version}" PARENT_SCOPE)
    endif()
endfunction()

if(GIT_FOUND AND EXISTS "${SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE tag_result
        OUTPUT_VARIABLE latest_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(tag_result EQUAL 0 AND NOT latest_tag STREQUAL "")
        string(REGEX REPLACE "^v" "" version "${latest_tag}")

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list "${latest_tag}..HEAD" --count
            WORKING_DIRECTORY "${SOURCE_DIR}"
            RESULT_VARIABLE count_result
            OUTPUT_VARIABLE commit_count
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(NOT count_result EQUAL 0 OR commit_count STREQUAL "")
            set(commit_count "0")
        endif()
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list HEAD --count
            WORKING_DIRECTORY "${SOURCE_DIR}"
            RESULT_VARIABLE count_result
            OUTPUT_VARIABLE commit_count
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(NOT count_result EQUAL 0 OR commit_count STREQUAL "")
            set(commit_count "0")
        endif()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE dirty_result
        OUTPUT_VARIABLE dirty_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(dirty_result EQUAL 0 AND NOT dirty_output STREQUAL "")
        set(dirty_suffix "+dirty")
    endif()
endif()

if(NOT commit_count STREQUAL "0" OR NOT dirty_suffix STREQUAL "")
    next_patch_version("${version}" next_version)
    set(version "${next_version}~dev${commit_count}")
endif()

set(version "${version}${dirty_suffix}")

set(content "#pragma once\n\n#define MTERM_VERSION \"${version}\"\n")

if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" existing_content)
else()
    set(existing_content "")
endif()

if(NOT existing_content STREQUAL content)
    get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    file(WRITE "${OUTPUT_FILE}" "${content}")
endif()
