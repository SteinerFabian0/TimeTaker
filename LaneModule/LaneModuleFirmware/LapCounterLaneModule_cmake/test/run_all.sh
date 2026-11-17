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

CFLAGS="-std=gnu11 -O1 -Wall -Wextra -Werror -DRELEASE"
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

# lane_protocol.h (guide §5) joins here as a byte-compare against the
# mainboard's copy once both exist, like the hub's shared_config.h check.

suite host_sync_time "$LANE/app/modules/sync_time.c"
suite host_led       "$LANE/app/modules/led_pattern.c" "$LANE/app/modules/indicator_led.c"

if [ "$failed" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "$failed SUITE(S) FAILED"
fi
exit "$failed"
