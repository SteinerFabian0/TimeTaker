# Firmware identity: the release number here, plus the git revision stamped on
# every build, so an image always names the exact source it was built from.
# The number moves only as a release step. NODE_LIST reports it (fwMajor/
# fwMinor/fwPatch/fwGitRev), and the full string is kept in the image for SWD
# reads.
set(CTRL_FW_VERSION "0.1.0")

set(CTRL_FW_VERSION_DIR ${CMAKE_CURRENT_BINARY_DIR}/version)

# No OUTPUT, so it runs on every build; the script rewrites the header only
# when its content changes, so an untouched tree does not relink.
add_custom_target(ctrl_fw_version
  COMMAND ${CMAKE_COMMAND}
          -DCTRL_FW_VERSION=${CTRL_FW_VERSION}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${CTRL_FW_VERSION_DIR}/ctrl_fw_version.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_stamp.cmake
  BYPRODUCTS ${CTRL_FW_VERSION_DIR}/ctrl_fw_version.h
  VERBATIM)

add_dependencies(${CMAKE_PROJECT_NAME} ctrl_fw_version)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CTRL_FW_VERSION_DIR})
