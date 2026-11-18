# TimeTaker BLE bridge firmware (nRF52805, NCS v3.4.2).
# BLE_FIRMWARE_GUIDE.md §8. Build: see tools/build.sh or §4.1.
cmake_minimum_required(VERSION 3.20.0)

# The custom board (boards/timetaker/tt_ble) and the stm-lines binding
# (dts/bindings) live in this folder.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Stage B of the RAM budget (guide §9, numbers in §14): the release image
# uses every fragment listed here. -DTT_STAGE_B_CONF="" builds stage A.
set(TT_STAGE_B_CONF "stage_b_mtu.conf;stage_b_dle.conf;stage_b_phy.conf"
    CACHE STRING "Stage B Kconfig fragments (empty = stage A)")
list(APPEND EXTRA_CONF_FILE ${TT_STAGE_B_CONF})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(timetaker_ble)

# The link codec is the control module's frame_codec, compiled from its tree.
# Do not copy it: one file, one set of host tests (guide §5.1).
set(CTRL_APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../SideModuleFirmware/LapCounterControlModule_cmake/app)

target_sources(app PRIVATE
  src/main.c
  src/ble_link.c
  src/stm_link.c
  src/bind_ui.c
  src/stm_ctrl.c
  # Pure modules (host-tested in test/)
  src/button_classifier.c
  src/led_table.c
  src/data_slots.c
  src/link_msg.c
  src/byte_ring.c
  ${CTRL_APP_DIR}/modules/frame_codec.c
)

# Reserve exactly the configured DATA slots (data_slots.h).
target_compile_definitions(app PRIVATE SLOTS_MAX=${CONFIG_TT_DATA_SLOTS}U)

# src first: the control tree's modules folder has headers with generic names.
target_include_directories(app PRIVATE src)
target_include_directories(app PRIVATE ${CTRL_APP_DIR}/modules)

include(cmake/version.cmake)
