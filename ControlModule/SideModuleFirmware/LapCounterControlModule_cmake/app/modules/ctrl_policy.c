/*
 * Coordinator policy. See ctrl_policy.h.
 */

#include "ctrl_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "host_protocol.h"

/* --- Sequence ------------------------------------------------------------- */

void ctrlPolicySeqStep(const ctrlPolicySeqIn_t *in, ctrlPolicySeqStep_t *out)
{
    memset(out, 0, sizeof(*out));
    if (in->state == (uint8_t)CTRL_POLICY_SEQ_IDLE) {
        return;
    }
    if (!in->powered) {
        out->abortReason = (uint8_t)HP_ABORT_POWER;
        return;
    }
    if ((in->configuredMask & in->seqLanes) != in->seqLanes) {
        out->abortReason = (uint8_t)HP_ABORT_LANE_LEFT;
        return;
    }

    bool armed = in->state == (uint8_t)CTRL_POLICY_SEQ_ARMED;
    if (in->state == (uint8_t)CTRL_POLICY_SEQ_ARMING) {
        if ((in->failedMask & in->seqLanes) != 0U) {
            out->abortReason = (uint8_t)HP_ABORT_LANE_FAILED;
            return;
        }
        if ((in->okMask & in->seqLanes) == in->seqLanes) {
            out->armed = true;
            armed      = true;
        } else if (in->nowUs + (uint64_t)CTRL_POLICY_SEQ_CONFIRM_MS * 1000U >= in->t0Us) {
            out->abortReason = (uint8_t)HP_ABORT_LANE_FAILED;
            return;
        }
    }

    if (armed && in->armSampling && !in->armApplied && in->nowUs >= in->t0Us) {
        out->applyArmSampling = true;
    }
    if (armed && in->nowUs >= in->toutUs) {
        out->lightsOut = true;
    }
}

/* --- BLE bind policy -------------------------------------------------------- */

void ctrlPolicyBleHello(bool listed, bool bindable, bool windowOpen, bool timerRunning,
                        uint32_t timerSinceMs, uint32_t nowMs, ctrlPolicyHello_t *out)
{
    memset(out, 0, sizeof(*out));
    if (bindable && listed) {
        out->bindState = (uint8_t)HP_BIND_BOUND;
        out->add       = true;                     /* TOUCHED: LRU stamp and name */
    } else if (bindable && windowOpen) {
        out->bindState   = (uint8_t)HP_BIND_BOUND;
        out->add         = true;
        out->cueAccepted = true;
    } else {
        out->bindState      = (uint8_t)HP_BIND_UNBOUND;
        out->unbound        = true;
        out->unboundSinceMs = timerRunning ? timerSinceMs : nowMs;
    }
}

bool ctrlPolicyKickDue(bool unbound, bool sessionOpen, uint32_t sinceMs, uint32_t nowMs, uint32_t graceMs)
{
    return unbound && sessionOpen && (uint32_t)(nowMs - sinceMs) >= graceMs;
}

void ctrlPolicyBleDown(bool linkWasUp, ctrlPolicyBleDown_t *out)
{
    out->resetSession = true;
    out->publishDown  = linkWasUp;
}

uint16_t ctrlPolicyWindowLeftS(bool open, uint32_t endMs, uint32_t nowMs)
{
    if (!open) {
        return 0U;
    }
    const int32_t leftMs = (int32_t)(endMs - nowMs);
    return (leftMs <= 0) ? 0U : (uint16_t)(((uint32_t)leftMs + 999U) / 1000U);
}

uint32_t ctrlPolicyBindAllowS(uint16_t windowS, uint32_t maxS)
{
    return ((uint32_t)windowS > maxS) ? maxS : (uint32_t)windowS;
}

/* --- Panel -------------------------------------------------------------------- */

ctrlPolicyLink_e ctrlPolicyPanelLink(bool anySessionOpen, bool anyLinkUp)
{
    if (anySessionOpen) {
        return CTRL_POLICY_LINK_SESSION;
    }
    return anyLinkUp ? CTRL_POLICY_LINK_NO_SESSION : CTRL_POLICY_LINK_NO_HOST;
}

bool ctrlPolicyButtonAborts(uint8_t button, uint8_t press, bool seqRunning)
{
    return button == (uint8_t)HP_BUTTON_BTN2 && press == (uint8_t)HP_PRESS_LONG && seqRunning;
}

uint8_t ctrlPolicyBleNodeState(uint8_t bleBridge)
{
    switch (bleBridge) {
    case HP_BLE_BRIDGE_ABSENT: return (uint8_t)HP_NODE_STATE_ABSENT;
    case HP_BLE_BRIDGE_OK:     return (uint8_t)HP_NODE_STATE_OK;
    default:                   return (uint8_t)HP_NODE_STATE_FAULT;
    }
}

int8_t ctrlPolicyBleRssi(uint8_t bleBridge, bool haveStats, int8_t statsRssi)
{
    return (bleBridge == (uint8_t)HP_BLE_BRIDGE_OK && haveStats) ? statsRssi : (int8_t)127;
}
