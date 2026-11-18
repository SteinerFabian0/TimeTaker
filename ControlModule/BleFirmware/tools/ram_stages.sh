#!/bin/sh
# Builds every RAM stage of BLE_FIRMWARE_GUIDE.md §9 into build-stage-*/ and
# prints flash/RAM and the free RAM of each. Stage B items are cumulative, in
# the guide's order (MTU, then DLE, then 2M PHY). Keep a stage only if
# >= 1536 B of RAM stay free.
HERE=$(cd "$(dirname "$0")/.." && pwd)
RAM_TOTAL=24576
for stage in "A:" "A+MTU:stage_b_mtu.conf" "A+MTU+DLE:stage_b_mtu.conf;stage_b_dle.conf" \
             "A+MTU+DLE+2M:stage_b_mtu.conf;stage_b_dle.conf;stage_b_phy.conf"; do
    name=${stage%%:*}
    conf=${stage#*:}
    dir=build-stage-$(echo "$name" | tr '+' '_')
    log=$(BUILD="$dir" "$HERE/tools/build.sh" -p always -- "-DTT_STAGE_B_CONF=$conf" 2>&1)
    if [ $? -ne 0 ]; then
        echo "$name: BUILD FAILED"; echo "$log" | tail -20; continue
    fi
    flash=$(echo "$log" | awk '/FLASH:/ {print $2}')
    ram=$(echo "$log" | awk '/RAM:/ {print $2}')
    echo "$name: flash $flash B, RAM $ram B, free RAM $((RAM_TOTAL - ram)) B"
done
