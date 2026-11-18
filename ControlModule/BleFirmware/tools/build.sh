#!/bin/sh
# Builds the BLE bridge firmware with the NCS v3.4.2 toolchain, from any shell.
#
#   tools/build.sh                         release image -> build/zephyr/zephyr.hex
#   BUILD=build-debug tools/build.sh -- -DEXTRA_CONF_FILE=debug.conf
#   tools/build.sh -p always               any west build argument passes through
#
# The stage B fragments come from CMakeLists.txt (TT_STAGE_B_CONF). The same
# build from the toolchain shell, guide §4.1:
#   west build -b tt_ble/nrf52805 --no-sysbuild -d build .
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
NRFUTIL=${NRFUTIL:-$HOME/.local/bin/nrfutil}
BUILD=${BUILD:-build}
export ZEPHYR_BASE=${ZEPHYR_BASE:-$HOME/ncs/v3.4.2/zephyr}
cd "$HERE"
exec "$NRFUTIL" sdk-manager toolchain launch --ncs-version v3.4.2 -- \
    west build -b tt_ble/nrf52805 --no-sysbuild -s "$HERE" -d "$BUILD" "$@"
