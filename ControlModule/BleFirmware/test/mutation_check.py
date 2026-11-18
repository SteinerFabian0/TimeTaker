#!/usr/bin/env python3
"""Mutation check for the host suites.

Each mutation breaks one rule in a copy of src/, then runs run_all.sh against
the copy (SRC=...). A mutation counts as caught only if the suites still
COMPILE and at least one check FAILS; a build error is reported as such, not
as a catch. Exit status: the number of mutations not caught.

    test/mutation_check.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

TEST = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(TEST), "src")

# (name, file, old, new)
MUTATIONS = [
    # button_classifier.c (guide §7.1)
    ("button: dead zone starts at 1 s inclusive -> exclusive", "button_classifier.c",
     "if (ticks < BTN_DEAD_MIN_TICKS) {", "if (ticks <= BTN_DEAD_MIN_TICKS) {"),
    ("button: no debounce", "button_classifier.c",
     "if (b->sameCount < BTN_DEBOUNCE_SAMPLES || pressed == b->stable) {",
     "if (pressed == b->stable) {"),
    ("button: stuck press reported as VERY_LONG", "button_classifier.c",
     "return BTN_EVT_NONE;                     /* stuck button */", "return BTN_EVT_VERY_LONG;"),
    ("button: hold counted from 1, not 0", "button_classifier.c",
     "b->heldTicks = 0U;", "b->heldTicks = 1U;"),
    # led_table.c (guide §7.2)
    ("led: pair window before BLE off (rows 3/4 swapped)", "led_table.c",
     "    if (in->advertiseOff) {\n        return LED_PAT_OFF;\n    }\n    if (in->pairWindow) {\n        return LED_PAT_FAST_BLINK;\n    }",
     "    if (in->pairWindow) {\n        return LED_PAT_FAST_BLINK;\n    }\n    if (in->advertiseOff) {\n        return LED_PAT_OFF;\n    }"),
    ("led: hold beats cue", "led_table.c",
     "if (in->cue != LED_CUE_NONE) {", "if (in->cue != LED_CUE_NONE && in->hold == LED_HOLD_NONE) {"),
    ("led: BOUND shown without a connection", "led_table.c",
     "if (in->connected) {", "if (in->connected || in->session == BLE_UI_SESSION_BOUND) {"),
    ("led: fast blink not 5 Hz", "led_table.c",
     "stepsFastBlink[]    = { 2, 2 };", "stepsFastBlink[]    = { 2, 3 };"),
    ("led: pattern change keeps the old phase", "led_table.c",
     "        r->phase   = 0U;", "        r->phase  += elapsed;"),
    # data_slots.c (guide §5.2, §5.4)
    ("slots: credit before the last completion", "data_slots.c",
     "if (sl->used && sl->pending == 0U && (sl->failed || sl->queued >= sl->len)) {",
     "if (sl->used && (sl->failed || sl->queued >= sl->len)) {"),
    ("slots: owed credits treated as held by the STM", "data_slots.c",
     "(uint16_t)(s->orderCount + s->owed) >= s->n", "s->orderCount >= s->n"),
    ("slots: drop-and-credit unbounded", "data_slots.c",
     "if (s->owed >= slotsFree(s)) {", "if (s->owed >= 255U) {"),
    ("slots: hello grants n, not the free slots", "data_slots.c",
     "uint8_t slotsHelloCredits(const dataSlots_t *s)\n{\n    return slotsFree(s);",
     "uint8_t slotsHelloCredits(const dataSlots_t *s)\n{\n    return s->n;"),
    ("slots: lowest slot first, not arrival order", "data_slots.c",
     "const uint8_t     idx = s->order[i];", "const uint8_t     idx = i;"),
    ("slots: disconnect keeps the in-flight count", "data_slots.c",
     "    s->inFlight = 0U;\n}", "}"),
    # link_msg.c (guide §5.3)
    ("link: CTRL one byte short accepted", "link_msg.c",
     "if ((uint16_t)(len - 1U) < need) {", "if ((uint16_t)(len - 1U) < need - 1U) {"),
    ("link: longer CTRL rejected", "link_msg.c",
     "if ((uint16_t)(len - 1U) < need) {", "if ((uint16_t)(len - 1U) != need) {"),
    ("link: nRF->STM types accepted at the nRF", "link_msg.c",
     "if (need == 0U || !isStmToNrf(type)) {", "if (need == 0U || !isStmToNrf(0x41U)) {"),
    ("link: DATA 245 B accepted", "link_msg.c",
     "bodyLen > BLE_LINK_DATA_MAX) {", "bodyLen > BLE_LINK_DATA_MAX + 1U) {"),
    ("link: empty DATA accepted", "link_msg.c",
     "if (bodyLen == 0U || bodyLen > BLE_LINK_DATA_MAX) {", "if (bodyLen > BLE_LINK_DATA_MAX) {"),
    ("link: CUE kind not checked", "link_msg.c",
     "if (out->u.cue.kind != BLE_CUE_BIND_ACCEPTED && out->u.cue.kind != BLE_CUE_BINDS_CLEARED) {",
     "if (out->u.cue.kind == 0xEEU) {"),
    ("link: UI_STATE pairWindow not checked", "link_msg.c",
     " || out->u.uiState.pairWindow > 1U) {", ") {"),
    ("link: KICK passes any reason", "link_msg.c",
     "        return LINK_KICK_DEFAULT_REASON;", "        return stmReason;"),
    ("link: CTRL encoded on the DATA channel", "link_msg.c",
     "const uint8_t head[2] = { BLE_LINK_CH_CTRL, type };", "const uint8_t head[2] = { BLE_LINK_CH_DATA, type };"),
    ("link: no reset bit reported as UNKNOWN", "link_msg.c",
     "return BLE_RESET_POWER_ON;", "return BLE_RESET_UNKNOWN;"),
    ("link: PIN checked before WATCHDOG", "link_msg.c",
     "    if ((resetreas & NRF_RESETREAS_DOG) != 0U) {\n        return BLE_RESET_WATCHDOG;\n    }\n",
     "    if ((resetreas & NRF_RESETREAS_RESETPIN) != 0U) {\n        return BLE_RESET_PIN;\n    }\n"
     "    if ((resetreas & NRF_RESETREAS_DOG) != 0U) {\n        return BLE_RESET_WATCHDOG;\n    }\n"),
    ("link: sat16 wraps", "link_msg.c",
     "return (v > 0xFFFFU) ? 0xFFFFU : (uint16_t)v;", "return (uint16_t)v;"),
    # byte_ring.c
    ("ring: holds size-1 bytes", "byte_ring.c",
     "const uint16_t room = (uint16_t)(r->size - (uint16_t)(head - LOAD_ACQ(&r->tail)));",
     "const uint16_t room = (uint16_t)(r->size - 1U - (uint16_t)(head - LOAD_ACQ(&r->tail)));"),
    ("ring: peek runs past the end of the buffer", "byte_ring.c",
     "return (avail < toEnd) ? avail : toEnd;", "return (avail <= r->size) ? avail : toEnd;"),
    ("ring: writeAll writes partially", "byte_ring.c",
     "if (len > byteRingFree(r)) {", "if (len > r->size) {"),
    ("ring: non power-of-two size accepted", "byte_ring.c",
     " || (size & (size - 1U)) != 0U) {", ") {"),
]


def main():
    missed = 0
    for name, fname, old, new in MUTATIONS:
        work = tempfile.mkdtemp()
        try:
            copy = os.path.join(work, "src")
            shutil.copytree(SRC, copy)
            path = os.path.join(copy, fname)
            text = open(path).read()
            if text.count(old) < 1:
                print(f"STALE   {name}: pattern not found in {fname}")
                missed += 1
                continue
            open(path, "w").write(text.replace(old, new, 1))
            env = dict(os.environ, SRC=copy)
            res = subprocess.run([os.path.join(TEST, "run_all.sh")], env=env,
                                 capture_output=True, text=True)
            out = res.stdout + res.stderr
            fails = len(re.findall(r"^.* FAIL$", out, re.M))
            if "error:" in out:
                print(f"BUILD   {name}: mutation does not compile (not a catch)")
                missed += 1
            elif fails == 0 and res.returncode != 0 and "checks," in out:
                print(f"caught  {name}: a suite crashed")
            elif fails == 0:
                print(f"MISSED  {name}")
                missed += 1
            else:
                print(f"caught  {name}: {fails} failing check(s)")
        finally:
            shutil.rmtree(work)
    print(f"{len(MUTATIONS) - missed}/{len(MUTATIONS)} mutations caught")
    return missed


if __name__ == "__main__":
    sys.exit(main())
