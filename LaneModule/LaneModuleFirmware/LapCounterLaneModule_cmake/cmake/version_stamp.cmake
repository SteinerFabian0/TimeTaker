# Build-time step of cmake/version.cmake: writes lane_fw_version.h.
# Dirty = anything in this tree differs from HEAD (tracked or untracked,
# .gitignore respected): such an image cannot be rebuilt from its hash. A tree
# outside git counts as dirty for the same reason.

execute_process(COMMAND git rev-parse --short=7 HEAD
                WORKING_DIRECTORY ${SRC}
                OUTPUT_VARIABLE rev
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE revStatus
                ERROR_QUIET)

set(dirty 0)
if(NOT revStatus EQUAL 0)
  set(rev "nogit")
  set(dirty 1)
else()
  execute_process(COMMAND git status --porcelain -- .
                  WORKING_DIRECTORY ${SRC}
                  OUTPUT_VARIABLE changes
                  ERROR_QUIET)
  if(NOT changes STREQUAL "")
    set(dirty 1)
    string(APPEND rev "-dirty")
  endif()
endif()

set(content "/* Generated at build time by cmake/version_stamp.cmake. Do not edit. */
#pragma once

#define LANE_FW_VERSION_STRING  \"${LANE_FW_VERSION}+${rev}\"
#define LANE_FW_TREE_DIRTY      ${dirty}
")

set(old "")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
endif()
if(NOT old STREQUAL content)
  file(WRITE ${OUT} "${content}")
endif()
