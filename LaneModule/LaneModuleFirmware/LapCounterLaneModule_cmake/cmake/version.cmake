# Firmware identity: the release number here, plus the git revision stamped on
# every build, so an image always names the exact source it was built from.
# The number moves only as a release step (docs/RELEASING.md). The string goes
# out over the bus in the ID register.
set(LANE_FW_VERSION "0.1.0")

set(LANE_FW_VERSION_DIR ${CMAKE_CURRENT_BINARY_DIR}/version)

# No OUTPUT, so it runs on every build; the script rewrites the header only
# when its content changes, so an untouched tree does not relink.
add_custom_target(lane_fw_version
  COMMAND ${CMAKE_COMMAND}
          -DLANE_FW_VERSION=${LANE_FW_VERSION}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${LANE_FW_VERSION_DIR}/lane_fw_version.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_stamp.cmake
  BYPRODUCTS ${LANE_FW_VERSION_DIR}/lane_fw_version.h
  VERBATIM)

add_dependencies(${CMAKE_PROJECT_NAME} lane_fw_version)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${LANE_FW_VERSION_DIR})
