#
# Runs at every build. Generates version.h from `git describe` — but only
# overwrites the file when the content actually changes, so TUs that #include
# it don't needlessly recompile.
#
# Inputs: SOURCE_DIR (repo root), OUTPUT (path to version.h to write).
#
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(GIT_DESCRIBE MATCHES "^v")
    string(SUBSTRING "${GIT_DESCRIBE}" 1 -1 VERSION_STRING)
elseif(GIT_DESCRIBE)
    set(VERSION_STRING "0.1.0-${GIT_DESCRIBE}")
else()
    set(VERSION_STRING "0.1.0")
endif()

set(NEW_CONTENT "#pragma once\n#define GRIT_VERSION \"${VERSION_STRING}\"\n")

set(OLD_CONTENT "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" OLD_CONTENT)
endif()

if(NOT OLD_CONTENT STREQUAL NEW_CONTENT)
    file(WRITE "${OUTPUT}" "${NEW_CONTENT}")
    message(STATUS "Version: ${VERSION_STRING}")
endif()
