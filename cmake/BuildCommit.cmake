# Writes the commit being built into a header (only when it changes, so nothing
# rebuilds for nothing). The updater compares it with the commit of the newest
# published build. Run as: cmake -DSOURCE_DIR=... -DOUT=... -P BuildCommit.cmake
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE RESULT)
if(NOT RESULT EQUAL 0)
    set(COMMIT "")
endif()
set(CONTENT "#pragma once\n// Written by cmake/BuildCommit.cmake.\n#define FASTCHAT_BUILD_COMMIT \"${COMMIT}\"\n")
set(OLD "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" OLD)
endif()
if(NOT OLD STREQUAL CONTENT)
    file(WRITE "${OUT}" "${CONTENT}")
endif()
