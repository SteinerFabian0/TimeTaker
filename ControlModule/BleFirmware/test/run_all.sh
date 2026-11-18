#!/bin/sh
# Host tests: the BLE bridge's pure logic, compiled natively from the real
# src/ files (and the control tree's frame_codec.c, which the firmware
# compiles directly). Suites and rules: BLE_FIRMWARE_GUIDE.md §11.1, §14.
# Mutation check of the suites: test/mutation_check.py.
#
#   test/run_all.sh                 all suites
#   SRC=/some/copy test/run_all.sh  test another copy of src/ (mutation_check.sh)
#
# Exit status is the number of failed suites.

TEST=$(cd "$(dirname "$0")" && pwd)
BLE=$(cd "$TEST/.." && pwd)
ROOT=$(cd "$BLE/../.." && pwd)
SRC=${SRC:-$BLE/src}
CODEC=$ROOT/ControlModule/SideModuleFirmware/LapCounterControlModule_cmake/app/modules
CC=${CC:-cc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-std=gnu11 -O1 -Wall -Wextra -Werror -DRELEASE"
# Only frame_codec.h is used from the control tree; src/ comes first.
INC="-I$SRC -I$CODEC"

failed=0

# suite <name> <sources...>: build <name>/main.c with them, run it.
suite()
{
    name=$1
    shift
    echo "== $name"
    if $CC $CFLAGS $INC -o "$OUT/$name" "$TEST/$name/main.c" "$@" \
       && "$OUT/$name"; then
        :
    else
        failed=$((failed + 1))
    fi
    echo
}

# The link contract must be byte-identical with the STM tree's copy.
echo "== ble_link_protocol.h copy check"
if cmp "$BLE/src/ble_link_protocol.h" \
       "$ROOT/ControlModule/SideModuleFirmware/LapCounterControlModule_cmake/app/config/ble_link_protocol.h"; then
    echo "identical"
else
    failed=$((failed + 1))
fi
echo

suite host_button    "$SRC/button_classifier.c"
suite host_led       "$SRC/led_table.c"
suite host_slots     "$SRC/data_slots.c"
suite host_link_msg  "$SRC/link_msg.c" "$CODEC/frame_codec.c"
suite host_byte_ring "$SRC/byte_ring.c"

# The bench link tool's decoder against the reference encoder.
echo "== tools/link_tool.py selftest"
if python3 "$BLE/tools/link_tool.py" selftest; then
    :
else
    failed=$((failed + 1))
fi
echo

if [ "$failed" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "$failed SUITE(S) FAILED"
fi
exit "$failed"
