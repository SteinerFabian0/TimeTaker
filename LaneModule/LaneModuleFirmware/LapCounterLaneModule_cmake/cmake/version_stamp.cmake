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

# Numbers for the ID register: MAJOR.MINOR.PATCH and the short revision as a
# u32 (0 outside git).
string(REPLACE "." ";" parts "${LANE_FW_VERSION}")
list(GET parts 0 major)
list(GET parts 1 minor)
list(GET parts 2 patch)
set(revHex "0")
if(revStatus EQUAL 0)
  string(SUBSTRING "${rev}" 0 7 revHex)
endif()

set(content "/* Generated at build time by cmake/version_stamp.cmake. Do not edit. */
#pragma once

#define LANE_FW_VERSION_STRING  \"${LANE_FW_VERSION}+${rev}\"
#define LANE_FW_TREE_DIRTY      ${dirty}
#define LANE_FW_VERSION_MAJOR   ${major}U
#define LANE_FW_VERSION_MINOR   ${minor}U
#define LANE_FW_VERSION_PATCH   ${patch}U
#define LANE_FW_GIT_REV         0x${revHex}UL
")

set(old "")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
endif()
if(NOT old STREQUAL content)
  file(WRITE ${OUT} "${content}")
endif()
