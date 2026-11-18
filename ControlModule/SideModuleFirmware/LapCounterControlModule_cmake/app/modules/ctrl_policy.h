/*
 * Coordinator policy: the decisions of sequence_manager, host_manager and
 * panel_manager that need no hardware, kept apart so the host tests can hold
 * them (the coordinators keep the HAL calls, the timers and the side
 * effects). Pure logic: no HAL, no globals. Host-tested
 * (test/host_ctrl_policy).
 *
 * Sequence (sequence_manager, CONTROL_FIRMWARE_GUIDE §5.3), one decision per
 * super-loop pass while a start sequence runs, in this order:
 *   1. lane power off                         -> abort POWER
 *   2. a participating lane not CONFIGURED    -> abort LANE_LEFT
 *   3. ARMING: a lane's schedule FAILED       -> abort LANE_FAILED
 *              every lane confirmed            -> ARMED
 *              not all by T0 - CONFIRM         -> abort LANE_FAILED (better
 *                                                 than a dark lane)
 *   4. T0 passed with armSampling             -> the lanes' table mode
 *                                                 becomes ON (once), so a
 *                                                 re-join or a BUS_IDLE_SAFE
 *                                                 re-push keeps them sampling
 *   5. Tout passed                            -> EVT_LIGHTS_OUT, idle
 *
 * BLE bind policy (host_manager, SYSTEM_ARCHITECTURE §9.5):
 *   HELLO   listed -> BOUND (LRU touched); window open -> added, BOUND, cue;
 *           else UNBOUND. The 60 s grace starts at the FIRST unbound HELLO of
 *           a connection; the app's repeated HELLOs do not restart it. An
 *           all-zero clientId can never be bound.
 *   grace   an UNBOUND open session is kicked HOST_MANAGER_UNBOUND_GRACE_MS
 *           after its first HELLO.
 *   down    a disconnect reaches the STM twice (BLE_IRQ fall and LINK_DOWN,
 *           either order). EVERY report resets the BLE session (idempotent);
 *           only the first after an UP publishes EVT_BLE_LINK DOWN. Gating
 *           the reset on a seen LINK_UP would leave the session open (and
 *           bound) for the next central when a LINK_UP frame was lost.
 *   window  seconds left, rounded up; BIND_ALLOW is clamped.
 *   bridge  NODE_LIST's BLE node follows STATUS bleBridge (nrf_link_proto's
 *           liveness rule): ABSENT / OK / FAULT (version mismatch).
 *
 * Panel (panel_manager): LED_STATUS shows session > link > nothing; BTN2 LONG
 * aborts a running start sequence (the EVT_BUTTON goes out as well).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* --- Sequence ------------------------------------------------------------- */

#define CTRL_POLICY_SEQ_CONFIRM_MS  20U   /* lanes confirm by T0 - this */

typedef enum {
    CTRL_POLICY_SEQ_IDLE = 0,
    CTRL_POLICY_SEQ_ARMING,      /* actions queued, waiting for every lane to confirm */
    CTRL_POLICY_SEQ_ARMED,       /* every lane confirmed, waiting for Tout */
} ctrlPolicySeqState_e;

typedef struct {
    uint8_t  state;              /* ctrlPolicySeqState_e */
    bool     powered;            /* lane rail on, SYNC running */
    uint8_t  configuredMask;     /* lanes CONFIGURED now (bit lane-1) */
    uint8_t  seqLanes;           /* lanes taking part */
    uint8_t  okMask;             /* lanes whose schedule read-back is OK */
    uint8_t  failedMask;         /* ... is FAILED */
    bool     armSampling;
    bool     armApplied;         /* step 4 already done */
    uint64_t nowUs;
    uint64_t t0Us;
    uint64_t toutUs;
} ctrlPolicySeqIn_t;

typedef struct {
    uint8_t abortReason;         /* hpSeqAbortReason_e, 0 = none; nothing else applies */
    bool    armed;               /* ARMING -> ARMED */
    bool    applyArmSampling;    /* set the lanes' table mode ON now */
    bool    lightsOut;           /* publish EVT_LIGHTS_OUT, back to idle */
} ctrlPolicySeqStep_t;

void ctrlPolicySeqStep(const ctrlPolicySeqIn_t *in, ctrlPolicySeqStep_t *out);

/* --- BLE bind policy -------------------------------------------------------- */

typedef struct {
    uint8_t  bindState;          /* hpBindState_e: BOUND or UNBOUND */
    bool     add;                /* bindListAdd (touch or add) */
    bool     cueAccepted;        /* CUE{BIND_ACCEPTED}: newly bound by the window */
    bool     unbound;            /* the grace timer runs */
    uint32_t unboundSinceMs;
} ctrlPolicyHello_t;

/*
 * listed: clientId on the bind list. bindable: clientId not all zero.
 * timerRunning / timerSinceMs: an unbound HELLO of this connection came
 * before.
 */
void ctrlPolicyBleHello(bool listed, bool bindable, bool windowOpen, bool timerRunning,
                        uint32_t timerSinceMs, uint32_t nowMs, ctrlPolicyHello_t *out);

/* The unbound session's grace is over: KICK. */
bool ctrlPolicyKickDue(bool unbound, bool sessionOpen, uint32_t sinceMs, uint32_t nowMs, uint32_t graceMs);

typedef struct {
    bool resetSession;           /* always */
    bool publishDown;            /* EVT_BLE_LINK DOWN: only the first report after an UP */
} ctrlPolicyBleDown_t;

void ctrlPolicyBleDown(bool linkWasUp, ctrlPolicyBleDown_t *out);

/* Pairing window seconds left, rounded up; 0 when closed or over. */
uint16_t ctrlPolicyWindowLeftS(bool open, uint32_t endMs, uint32_t nowMs);

/* BIND_ALLOW windowS clamped to maxS (0 closes). */
uint32_t ctrlPolicyBindAllowS(uint16_t windowS, uint32_t maxS);

/* NODE_LIST state of the BLE bridge from hpBleBridgeState_e (STATUS
 * bleBridge, same rule): ABSENT -> ABSENT, OK -> OK, VERSION_MISMATCH (or
 * anything unknown) -> FAULT. */
uint8_t ctrlPolicyBleNodeState(uint8_t bleBridge);

/* STATUS bleRssi: the last STATS rssi while the bridge is OK, else 127
 * (unknown): a silent nRF's last value would be stale. */
int8_t ctrlPolicyBleRssi(uint8_t bleBridge, bool haveStats, int8_t statsRssi);

/* --- Panel -------------------------------------------------------------------- */

typedef enum {
    CTRL_POLICY_LINK_UNKNOWN = 0,
    CTRL_POLICY_LINK_NO_HOST,      /* short blip every 2 s */
    CTRL_POLICY_LINK_NO_SESSION,   /* 1 Hz blink */
    CTRL_POLICY_LINK_SESSION,      /* steady */
} ctrlPolicyLink_e;

ctrlPolicyLink_e ctrlPolicyPanelLink(bool anySessionOpen, bool anyLinkUp);

/* button/press: hpButtonId_e / hpPressKind_e values. */
bool ctrlPolicyButtonAborts(uint8_t button, uint8_t press, bool seqRunning);
