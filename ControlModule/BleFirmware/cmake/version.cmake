# Firmware identity: the release number here, plus the git revision stamped on
# every build, so an image always names the exact source it was built from.
# NRF_HELLO reports it (fwMajor/fwMinor/fwPatch/fwGitRev), and the full string
# is kept in the image for SWD reads. Same scheme as the control module.
set(BLE_FW_VERSION "0.1.0")

set(BLE_FW_VERSION_DIR ${CMAKE_CURRENT_BINARY_DIR}/version)

# No OUTPUT, so it runs on every build; the script rewrites the header only
# when its content changes, so an untouched tree does not relink.
add_custom_target(ble_fw_version
  COMMAND ${CMAKE_COMMAND}
          -DBLE_FW_VERSION=${BLE_FW_VERSION}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${BLE_FW_VERSION_DIR}/fw_version.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_stamp.cmake
  BYPRODUCTS ${BLE_FW_VERSION_DIR}/fw_version.h
  VERBATIM)

add_dependencies(app ble_fw_version)
target_include_directories(app PRIVATE ${BLE_FW_VERSION_DIR})
