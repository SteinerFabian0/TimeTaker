#!/bin/sh
# Host tests: the lane's pure logic, compiled natively from the real app/
# sources against the shims in test/shim. See test/README.md.
#
#   test/run_all.sh
#
# Exit status is the number of failed suites.

TEST=$(cd "$(dirname "$0")" && pwd)
LANE=$(cd "$TEST/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-std=gnu11 -O1 -Wall -Wextra -Werror -DRELEASE -DLANE_HOST_TEST"
INC="-I$TEST/shim -I$LANE/app/config -I$LANE/app/modules"

failed=0

# suite <name> <app sources...>: build <name>/main.c with them, run it.
suite()
{
    name=$1
    shift
    echo "== $name"
    if cc $CFLAGS $INC -o "$OUT/$name" "$TEST/$name/main.c" "$TEST/shim/host_hal.c" "$@" \
       && "$OUT/$name"; then
        :
    else
        failed=$((failed + 1))
    fi
    echo
}

# lane_protocol.h (guide §5) is byte-identical in the control module's tree.
ROOT=$(cd "$LANE/../../.." && pwd)
echo "== lane_protocol.h copy check"
if cmp "$LANE/app/config/lane_protocol.h" \
       "$ROOT/ControlModule/SideModuleFirmware/LapCounterControlModule_cmake/app/config/lane_protocol.h"; then
    echo "identical"
else
    failed=$((failed + 1))
fi
echo

suite host_sync_time "$LANE/app/modules/sync_time.c"
suite host_led       "$LANE/app/modules/led_pattern.c" "$LANE/app/modules/indicator_led.c"
suite host_detect    "$LANE/app/modules/lane_detect.c"
suite host_event_fifo "$LANE/app/modules/event_fifo.c"
suite host_schedule  "$LANE/app/modules/schedule_queue.c"
suite host_regs      "$LANE/app/modules/lane_regs.c" "$LANE/app/modules/led_pattern.c" "$LANE/app/modules/image_crc.c"
suite host_chain     "$LANE/app/modules/chain_logic.c"

if [ "$failed" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "$failed SUITE(S) FAILED"
fi
exit "$failed"
