#!/bin/sh
# Runs every host test suite and firmware build in the repo. Exit status =
# number of failed steps. Usage: tools/check_all.sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=~/.local/share/stm32cube/bundles
PATH=$(ls -d $B/cmake/*/bin | tail -1):$(ls -d $B/ninja/*/bin | tail -1):$(ls -d $B/gnu-tools-for-stm32/*/bin | tail -1):$HOME/.local/bin:$PATH
export PATH
fail=0
step() { name=$1; shift; if out=$("$@" 2>&1); then echo "ok    $name"; else echo "FAIL  $name"; echo "$out" | tail -15; fail=$((fail+1)); fi; }
build() { # build <dir> <preset>: 0 warnings except the dirty-tree #warning
  ( cd "$1" && cmake --preset "$2" >/dev/null && cmake --build --preset "$2" 2>&1 ) > /tmp/tt_build.$$ 2>&1 || { cat /tmp/tt_build.$$; rm -f /tmp/tt_build.$$; return 1; }
  w=$(grep -E "warning:|error:" /tmp/tt_build.$$ | grep -v "uncommitted changes" ); rm -f /tmp/tt_build.$$
  [ -z "$w" ] || { echo "$w"; return 1; }
}
CTRL=$ROOT/ControlModule/SideModuleFirmware/LapCounterControlModule_cmake
LANE=$ROOT/LaneModule/LaneModuleFirmware/LapCounterLaneModule_cmake
step "protocol generator check"  python3 "$ROOT/protocol/gen_protocol.py" --check
step "control host tests"        "$CTRL/test/run_all.sh"
step "lane host tests"           "$LANE/test/run_all.sh"
step "BLE host tests"            "$ROOT/ControlModule/BleFirmware/test/run_all.sh"
step "web app tests"             sh -c "cd '$ROOT/WebApp' && npm test"
for p in debug_GCC_main release_GCC_main debug_GCC_boot; do step "control build $p" build "$CTRL" $p; done
LP=$(grep -o '"name": *"debug[^"]*"' "$LANE/CMakePresets.json" | head -1 | sed 's/.*"\(debug[^"]*\)"/\1/')
step "lane build $LP" build "$LANE" "$LP"
echo; [ $fail -eq 0 ] && echo "ALL CHECKS PASSED" || echo "$fail CHECK(S) FAILED"
exit $fail
