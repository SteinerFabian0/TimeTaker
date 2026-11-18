/*
 * Host test for ctrl_policy.c: the decisions of sequence_manager (per-pass
 * step of a running start sequence, armSampling at T0), host_manager (BLE
 * bind policy at HELLO, grace kick, disconnect dedupe, pairing window) and
 * panel_manager (status LED, BTN2 abort).
 */

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_policy.h"
#include "host_protocol.h"
#include "../test_check.h"

#define MS 1000ULL

static ctrlPolicySeqIn_t arming(void)
{
    ctrlPolicySeqIn_t in = {
        .state = CTRL_POLICY_SEQ_ARMING, .powered = true, .configuredMask = 0x3F, .seqLanes = 0x3F,
        .okMask = 0, .failedMask = 0, .armSampling = true, .armApplied = false,
        .nowUs = 1000 * MS, .t0Us = 1150 * MS, .toutUs = 10000 * MS,
    };
    return in;
}

static void testSequence(void)
{
    ctrlPolicySeqStep_t st;
    ctrlPolicySeqIn_t in = arming();

    in.state = CTRL_POLICY_SEQ_IDLE;
    ctrlPolicySeqStep(&in, &st);
    check("seq: idle -> nothing", st.abortReason == 0 && !st.armed && !st.applyArmSampling && !st.lightsOut);

    in = arming();
    ctrlPolicySeqStep(&in, &st);
    check("seq: arming, lanes not confirmed, T0 far -> wait", st.abortReason == 0 && !st.armed);

    in.okMask = 0x1F;
    ctrlPolicySeqStep(&in, &st);
    check("seq: five of six confirmed -> still waiting", st.abortReason == 0 && !st.armed);

    in.okMask = 0x3F;
    ctrlPolicySeqStep(&in, &st);
    check("seq: all confirmed -> ARMED", st.abortReason == 0 && st.armed && !st.applyArmSampling && !st.lightsOut);

    in = arming();
    in.okMask = 0x3F;
    in.configuredMask = 0x37;
    ctrlPolicySeqStep(&in, &st);
    check("seq: a lane left -> abort LANE_LEFT (before anything else)", st.abortReason == HP_ABORT_LANE_LEFT);

    in = arming();
    in.seqLanes = 0x07;
    in.configuredMask = 0x07;
    in.okMask = 0x07;
    ctrlPolicySeqStep(&in, &st);
    check("seq: only the participating lanes count", st.armed && st.abortReason == 0);

    in = arming();
    in.powered = false;
    in.configuredMask = 0;
    ctrlPolicySeqStep(&in, &st);
    check("seq: power off -> abort POWER (wins over LANE_LEFT)", st.abortReason == HP_ABORT_POWER);

    in = arming();
    in.failedMask = 0x04;
    ctrlPolicySeqStep(&in, &st);
    check("seq: a lane's read-back FAILED -> abort LANE_FAILED", st.abortReason == HP_ABORT_LANE_FAILED);

    in = arming();
    in.failedMask = 0x40;   /* not a participant */
    ctrlPolicySeqStep(&in, &st);
    check("seq: FAILED on a lane outside the sequence -> ignored", st.abortReason == 0);

    in = arming();
    in.nowUs = in.t0Us - CTRL_POLICY_SEQ_CONFIRM_MS * MS - 1U;
    ctrlPolicySeqStep(&in, &st);
    check("seq: unconfirmed 1 us before the confirm deadline -> wait", st.abortReason == 0);
    in.nowUs++;
    ctrlPolicySeqStep(&in, &st);
    check("seq: unconfirmed at T0 - 20 ms -> abort LANE_FAILED", st.abortReason == HP_ABORT_LANE_FAILED);

    in = arming();
    in.state = CTRL_POLICY_SEQ_ARMED;
    in.nowUs = in.t0Us - 1U;
    ctrlPolicySeqStep(&in, &st);
    check("seq: armed, before T0 -> nothing", !st.applyArmSampling && !st.lightsOut && st.abortReason == 0);
    in.nowUs = in.t0Us;
    ctrlPolicySeqStep(&in, &st);
    check("seq: T0 with armSampling -> table mode ON", st.applyArmSampling && !st.lightsOut);
    in.armApplied = true;
    ctrlPolicySeqStep(&in, &st);
    check("seq: ... only once", !st.applyArmSampling);
    in.armApplied = false;
    in.armSampling = false;
    ctrlPolicySeqStep(&in, &st);
    check("seq: no armSampling -> table untouched", !st.applyArmSampling);

    in = arming();
    in.state = CTRL_POLICY_SEQ_ARMED;
    in.nowUs = in.toutUs - 1U;
    ctrlPolicySeqStep(&in, &st);
    check("seq: 1 us before Tout -> lights still on", !st.lightsOut);
    in.nowUs = in.toutUs;
    ctrlPolicySeqStep(&in, &st);
    check("seq: Tout -> EVT_LIGHTS_OUT (and the T0 arm if still owed)", st.lightsOut && st.applyArmSampling);

    in = arming();
    in.okMask = 0x3F;
    in.nowUs = in.toutUs + 5U;
    ctrlPolicySeqStep(&in, &st);
    check("seq: confirmation and Tout in one late pass -> armed and lights out", st.armed && st.lightsOut);
}

static void testBind(void)
{
    ctrlPolicyHello_t h;
    ctrlPolicyBleHello(true, true, false, false, 0, 500, &h);
    check("hello: listed -> BOUND, LRU touched, no cue", h.bindState == HP_BIND_BOUND && h.add && !h.cueAccepted && !h.unbound);
    ctrlPolicyBleHello(false, true, true, false, 0, 500, &h);
    check("hello: window open -> added, BOUND, cue", h.bindState == HP_BIND_BOUND && h.add && h.cueAccepted);
    ctrlPolicyBleHello(true, true, true, false, 0, 500, &h);
    check("hello: listed and window open -> no cue (not new)", h.bindState == HP_BIND_BOUND && !h.cueAccepted);
    ctrlPolicyBleHello(false, true, false, false, 0, 500, &h);
    check("hello: unknown, window closed -> UNBOUND, grace from now",
          h.bindState == HP_BIND_UNBOUND && !h.add && h.unbound && h.unboundSinceMs == 500);
    ctrlPolicyBleHello(false, true, false, true, 100, 900, &h);
    check("hello: repeated unbound HELLO keeps the first grace start", h.unbound && h.unboundSinceMs == 100);
    ctrlPolicyBleHello(false, false, true, false, 0, 500, &h);
    check("hello: all-zero clientId -> never bound, even with the window open",
          h.bindState == HP_BIND_UNBOUND && !h.add && !h.cueAccepted);
    ctrlPolicyBleHello(false, true, false, true, 100, 900, &h);
    ctrlPolicyBleHello(true, true, false, true, 100, 900, &h);
    check("hello: bound later -> grace timer stops", !h.unbound);

    check("kick: grace not over", !ctrlPolicyKickDue(true, true, 1000, 60999, 60000));
    check("kick: grace over -> KICK", ctrlPolicyKickDue(true, true, 1000, 61000, 60000));
    check("kick: across the u32 tick wrap", ctrlPolicyKickDue(true, true, 0xFFFFFF00U, 60000, 60000));
    check("kick: session closed -> no kick", !ctrlPolicyKickDue(true, false, 0, 100000, 60000));
    check("kick: bound -> no kick", !ctrlPolicyKickDue(false, true, 0, 100000, 60000));

    ctrlPolicyBleDown_t d;
    ctrlPolicyBleDown(true, &d);
    check("down: first report after UP -> reset and publish", d.resetSession && d.publishDown);
    ctrlPolicyBleDown(false, &d);
    check("down: second report (or LINK_UP never seen) -> reset again, no event", d.resetSession && !d.publishDown);

    check("window: closed -> 0", ctrlPolicyWindowLeftS(false, 5000, 0) == 0);
    check("window: 59.001 s left -> 60", ctrlPolicyWindowLeftS(true, 60000, 999) == 60);
    check("window: 1 ms left -> 1", ctrlPolicyWindowLeftS(true, 1000, 999) == 1);
    check("window: over -> 0", ctrlPolicyWindowLeftS(true, 1000, 1000) == 0 && ctrlPolicyWindowLeftS(true, 1000, 5000) == 0);
    check("window: BIND_ALLOW clamped", ctrlPolicyBindAllowS(3600, 600) == 600 && ctrlPolicyBindAllowS(60, 600) == 60
          && ctrlPolicyBindAllowS(0, 600) == 0);
}

static void testPanel(void)
{
    check("panel: session open -> steady", ctrlPolicyPanelLink(true, true) == CTRL_POLICY_LINK_SESSION
          && ctrlPolicyPanelLink(true, false) == CTRL_POLICY_LINK_SESSION);
    check("panel: link, no session -> 1 Hz", ctrlPolicyPanelLink(false, true) == CTRL_POLICY_LINK_NO_SESSION);
    check("panel: nothing -> blip", ctrlPolicyPanelLink(false, false) == CTRL_POLICY_LINK_NO_HOST);
    check("panel: BTN2 LONG while running aborts", ctrlPolicyButtonAborts(HP_BUTTON_BTN2, HP_PRESS_LONG, true));
    check("panel: BTN2 LONG while idle does not", !ctrlPolicyButtonAborts(HP_BUTTON_BTN2, HP_PRESS_LONG, false));
    check("panel: BTN2 SHORT / VERY_LONG do not", !ctrlPolicyButtonAborts(HP_BUTTON_BTN2, HP_PRESS_SHORT, true)
          && !ctrlPolicyButtonAborts(HP_BUTTON_BTN2, HP_PRESS_VERY_LONG, true));
    check("panel: BTN1 / BTN3 LONG do not", !ctrlPolicyButtonAborts(HP_BUTTON_BTN1, HP_PRESS_LONG, true)
          && !ctrlPolicyButtonAborts(HP_BUTTON_BTN3, HP_PRESS_LONG, true));
}

static void testBridge(void)
{
    check("ble node: ABSENT -> ABSENT", ctrlPolicyBleNodeState(HP_BLE_BRIDGE_ABSENT) == HP_NODE_STATE_ABSENT);
    check("ble node: OK -> OK", ctrlPolicyBleNodeState(HP_BLE_BRIDGE_OK) == HP_NODE_STATE_OK);
    check("ble node: VERSION_MISMATCH -> FAULT",
          ctrlPolicyBleNodeState(HP_BLE_BRIDGE_VERSION_MISMATCH) == HP_NODE_STATE_FAULT);
    check("ble rssi: OK with STATS -> its rssi", ctrlPolicyBleRssi(HP_BLE_BRIDGE_OK, true, -60) == -60);
    check("ble rssi: no STATS yet -> 127", ctrlPolicyBleRssi(HP_BLE_BRIDGE_OK, false, -60) == 127);
    check("ble rssi: bridge ABSENT -> 127 (stale)", ctrlPolicyBleRssi(HP_BLE_BRIDGE_ABSENT, true, -60) == 127);
    check("ble rssi: VERSION_MISMATCH -> 127", ctrlPolicyBleRssi(HP_BLE_BRIDGE_VERSION_MISMATCH, true, -60) == 127);
}

int main(void)
{
    testBridge();
    testSequence();
    testBind();
    testPanel();
    return finish();
}
