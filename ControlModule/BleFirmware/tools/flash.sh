#!/bin/sh
# Flashes the BLE bridge with the ST-LINK V3 + xPack OpenOCD (no J-Link).
# BENCH_TESTS.md T1.
#
#   tools/flash.sh                 program build/zephyr/zephyr.hex, verify, run
#   tools/flash.sh --recover       mass erase + unlock first (APPROTECT, a new
#                                  module with a Raytac test image)
#   HEX=build-debug/zephyr/zephyr.hex tools/flash.sh
#
# After programming it makes sure UICR.APPROTECT reads 0x5A (HwDisabled).
# Newer nRF52 revisions lock the debug port at every reset unless that word
# says otherwise; the firmware (NCS default, NRF_APPROTECT_USE_UICR) copies it
# into APPROTECT.DISABLE at boot. OpenOCD's nrf52_recover leaves UICR erased
# (0xFFFFFFFF = protected), so without this step the next reset would lock
# SWD again. Release images keep it open too (guide §13 open question 3).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
OPENOCD=${OPENOCD:-$HOME/.local/bin/openocd}
HEX=${HEX:-$HERE/build/zephyr/zephyr.hex}
CFG=$HERE/boards/timetaker/tt_ble/support/openocd.cfg

RECOVER=""
if [ "$1" = "--recover" ]; then
    RECOVER="nrf52_recover;"
fi
[ -f "$HEX" ] || { echo "no image: $HEX (run tools/build.sh)"; exit 1; }

exec "$OPENOCD" -f "$CFG" -c "
init
$RECOVER
reset halt
program {$HEX} verify
set approtect [mrw 0x10001208]
if {\$approtect == 0xFFFFFFFF} {
    flash fillw 0x10001208 0x0000005A 1
    echo {UICR.APPROTECT written: 0x5A (debug port stays open)}
} elseif {\$approtect != 0x5A} {
    echo [format {UICR.APPROTECT = 0x%08X: not erased, cannot open it without --recover} \$approtect]
}
reset run
shutdown
"
