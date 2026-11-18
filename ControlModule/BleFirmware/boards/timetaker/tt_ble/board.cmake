# Flashing: ST-LINK V3 + OpenOCD only (there is no J-Link on the bench).
# support/openocd.cfg holds the interface and target; the openocd runner picks
# it up from the board folder.
#
#   west flash -d build --runner openocd --openocd ~/.local/bin/openocd
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
