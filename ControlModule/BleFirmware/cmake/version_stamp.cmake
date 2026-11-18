# Build-time step of cmake/version.cmake: writes fw_version.h.
# Dirty = anything in this tree differs from HEAD (tracked or untracked,
# .gitignore respected): such an image cannot be rebuilt from its hash. A tree
# outside git counts as dirty for the same reason, and reports git rev 0.

execute_process(COMMAND git rev-parse --short=7 HEAD
                WORKING_DIRECTORY ${SRC}
                OUTPUT_VARIABLE rev
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE revStatus
                ERROR_QUIET)

set(dirty 0)
if(NOT revStatus EQUAL 0)
  set(rev "nogit")
  set(revHex "0")
  set(dirty 1)
else()
  set(revHex "${rev}")
  execute_process(COMMAND git status --porcelain -- .
                  WORKING_DIRECTORY ${SRC}
                  OUTPUT_VARIABLE changes
                  ERROR_QUIET)
  if(NOT changes STREQUAL "")
    set(dirty 1)
    string(APPEND rev "-dirty")
  endif()
endif()

string(REPLACE "." ";" parts "${BLE_FW_VERSION}")
list(GET parts 0 major)
list(GET parts 1 minor)
list(GET parts 2 patch)

set(content "/* Generated at build time by cmake/version_stamp.cmake. Do not edit. */
#pragma once

#define BLE_FW_VERSION_STRING  \"${BLE_FW_VERSION}+${rev}\"
#define BLE_FW_VERSION_MAJOR   ${major}U
#define BLE_FW_VERSION_MINOR   ${minor}U
#define BLE_FW_VERSION_PATCH   ${patch}U
#define BLE_FW_GIT_REV         0x${revHex}UL
#define BLE_FW_TREE_DIRTY      ${dirty}
")

set(old "")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
endif()
if(NOT old STREQUAL content)
  file(WRITE ${OUT} "${content}")
endif()
