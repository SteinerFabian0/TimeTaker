#!/bin/sh
# Host tests: the control module's pure logic, compiled natively from the real
# app/ sources. See test/README.md.
#
#   test/run_all.sh
#
# Exit status is the number of failed suites.

TEST=$(cd "$(dirname "$0")" && pwd)
CTRL=$(cd "$TEST/.." && pwd)
ROOT=$(cd "$CTRL/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-std=gnu11 -O1 -Wall -Wextra -Werror -DRELEASE"
INC="-I$CTRL/app/config -I$CTRL/app/modules"

failed=0

# suite <name> <app sources...>: build <name>/main.c with them, run it.
suite()
{
    name=$1
    shift
    echo "== $name"
    if cc $CFLAGS $INC -o "$OUT/$name" "$TEST/$name/main.c" "$@" \
       && "$OUT/$name"; then
        :
    else
        failed=$((failed + 1))
    fi
    echo
}

# The generated protocol files must match host_protocol.toml, or every suite
# below would test a stale header.
echo "== generated protocol files"
if python3 "$ROOT/protocol/gen_protocol.py" --check; then
    :
else
    failed=$((failed + 1))
fi
echo

# Shared headers must be byte-identical with their other tree's copy.
# copy_check <file in app/config> <other copy>
copy_check()
{
    echo "== $1 copy check"
    if cmp "$CTRL/app/config/$1" "$2"; then
        echo "identical"
    else
        failed=$((failed + 1))
    fi
    echo
}
copy_check lane_protocol.h     "$ROOT/LaneModule/LaneModuleFirmware/LapCounterLaneModule_cmake/app/config/lane_protocol.h"
copy_check ble_link_protocol.h "$ROOT/ControlModule/BleFirmware/src/ble_link_protocol.h"

suite host_frame_codec "$CTRL/app/modules/frame_codec.c"
suite host_protocol    "$CTRL/app/modules/frame_codec.c"
suite host_event_log   "$CTRL/app/modules/event_log.c"
suite host_session     "$CTRL/app/modules/host_session.c" "$CTRL/app/modules/frame_codec.c" "$CTRL/app/modules/event_log.c"
suite host_bind_list   "$CTRL/app/modules/bind_list.c"
suite host_seq_planner "$CTRL/app/modules/seq_planner.c"
suite host_settings_record "$CTRL/app/modules/settings_record.c" "$CTRL/app/modules/crc32.c"
suite host_nrf_link    "$CTRL/app/modules/nrf_link_proto.c" "$CTRL/app/modules/frame_codec.c" \
                       "$CTRL/app/modules/byte_ring.c" "$CTRL/app/modules/stream_budget.c"
suite host_lane_logic  "$CTRL/app/modules/lane_logic.c"
suite host_update      "$CTRL/app/modules/image_header.c" "$CTRL/app/modules/update_writer.c" "$CTRL/app/modules/crc32.c"

if [ "$failed" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "$failed SUITE(S) FAILED"
fi
exit "$failed"
