/*
 * Host test for lane_logic.c: the lane bus rules lane_manager applies
 * (CONTROL_FIRMWARE_GUIDE §5.1): join index and LEFT/release masks, EVENT_FIFO
 * and RAW_FIFO parsing with early read ends, the EVT_CROSSING mapping, seq
 * gaps, health transitions, "configured", and the LANE_CONFIG_SET checks.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lane_logic.h"
#include "../test_check.h"

static void testJoin(void)
{
    laneJoinDecision_t d;
    laneLogicDecideJoin(0x00, 0x00, &d);
    check("join: first lane of an empty bus -> index 1", d.index == 1 && d.leftMask == 0 && d.releaseMask == 0);
    laneLogicDecideJoin(0x07, 0x07, &d);
    check("join: 1-3 answering -> index 4", d.index == 4 && d.leftMask == 0);
    laneLogicDecideJoin(0x3F, 0x03, &d);
    check("join: lane 3 reset, 3-6 silent -> index 3, 3-6 LEFT", d.index == 3 && d.leftMask == 0x3C && d.releaseMask == 0);
    laneLogicDecideJoin(0x3F, 0x0B, &d);
    check("join: lane 4 still answers behind the gap -> released and LEFT",
          d.index == 3 && d.leftMask == 0x3C && d.releaseMask == 0x08);
    laneLogicDecideJoin(0x3F, 0x3E, &d);
    check("join: lane 1 reset -> index 1, all LEFT, 2-6 released", d.index == 1 && d.leftMask == 0x3F && d.releaseMask == 0x3E);
    laneLogicDecideJoin(0x3F, 0x3F, &d);
    check("join: six lanes answering -> no slot for a 7th", d.index == 0 && d.leftMask == 0);
}

static void testFifo(void)
{
    uint8_t buf[LANE_FIFO_READ_BYTES];
    memset(buf, 0, sizeof(buf));
    laneFifoHeader_t h = { .count = 2, .remaining = 5 };
    memcpy(buf, &h, sizeof(h));
    laneEvent_t e0 = { .seq = 10, .type = LANE_EVT_CROSSING, .flags = 0x18, .tEntryUs = 81234567ULL, .dwell10us = 2150, .depth = 812 };
    laneEvent_t e1 = e0; e1.seq = 11;
    memcpy(buf + 4, &e0, 16);
    memcpy(buf + 20, &e1, 16);
    laneFifoHeader_t got;
    laneEvent_t ev[4];
    uint8_t n = 9;
    check("fifo: full-length read, 2 records",
          laneLogicParseFifo(buf, sizeof(buf), &got, ev, &n) == CTRL_OK && n == 2 && got.remaining == 5
          && ev[1].seq == 11 && ev[0].tEntryUs == 81234567ULL);
    check("fifo: early read end after the records -> same 2",
          laneLogicParseFifo(buf, 36, &got, ev, &n) == CTRL_OK && n == 2);
    check("fifo: read cut inside record 2 -> only record 1",
          laneLogicParseFifo(buf, 35, &got, ev, &n) == CTRL_OK && n == 1);
    check("fifo: shorter than the header -> ERR_BAD_LENGTH",
          laneLogicParseFifo(buf, 3, &got, ev, &n) == CTRL_ERR_BAD_LENGTH && n == 0);
    buf[0] = LANE_FIFO_READ_MAX + 1U;
    check("fifo: count above the maximum -> garbage, nothing taken",
          laneLogicParseFifo(buf, sizeof(buf), &got, ev, &n) == CTRL_ERR_INVALID_ARG && n == 0);
    buf[0] = 0;
    check("fifo: empty FIFO", laneLogicParseFifo(buf, sizeof(buf), &got, ev, &n) == CTRL_OK && n == 0);

    uint8_t raw[LANE_RAW_READ_BYTES];
    memset(raw, 0, sizeof(raw));
    laneRawHeader_t rh = { .count = 3, .chunkSeq = 7, .periodUs = 4000, .firstUs = 5000000ULL };
    memcpy(raw, &rh, sizeof(rh));
    uint16_t s[3] = { 4000, 3990, 1200 };
    memcpy(raw + 16, s, 6);
    laneRawHeader_t rgot;
    uint16_t samples[LANE_RAW_READ_MAX];
    check("raw: 3 samples", laneLogicParseRaw(raw, sizeof(raw), &rgot, samples, &n) == CTRL_OK && n == 3
          && samples[2] == 1200 && rgot.chunkSeq == 7);
    check("raw: cut inside sample 3 -> 2", laneLogicParseRaw(raw, 21, &rgot, samples, &n) == CTRL_OK && n == 2);
    raw[0] = LANE_RAW_READ_MAX + 1U;
    check("raw: count above the maximum -> refused", laneLogicParseRaw(raw, sizeof(raw), &rgot, samples, &n) == CTRL_ERR_INVALID_ARG);
    check("raw: shorter than the header -> refused", laneLogicParseRaw(raw, 15, &rgot, samples, &n) == CTRL_ERR_BAD_LENGTH);
}

static void testCrossing(void)
{
    laneEvent_t e = { .seq = 65535, .type = LANE_EVT_BLOCKED, .flags = 0x21, .tEntryUs = 0x123456789ULL, .dwell10us = 9, .depth = 8 };
    hpEvtCrossing_t c;
    memset(&c, 0xEE, sizeof(c));
    laneLogicCrossing(3, &e, &c);
    check("crossing: every field copied, reserved zero",
          c.lane == 3 && c.type == LANE_EVT_BLOCKED && c.flags == 0x21 && c.tEntryUs == 0x123456789ULL
          && c.laneSeq == 65535 && c.dwell10us == 9 && c.depth == 8 && c.reserved == 0 && c.reserved2 == 0);
    check("seq: in order -> 0", laneLogicSeqGap(5, 5) == 0);
    check("seq: two lost -> 2", laneLogicSeqGap(5, 7) == 2);
    check("seq: across the wrap", laneLogicSeqGap(65535, 1) == 2);
    check("seq: backwards -> >= 0x8000", laneLogicSeqGap(10, 3) >= 0x8000U);

    /* Duplicates after a cut read: dropped, wrap-aware, within the window. */
    check("seq check: nothing known yet -> take", laneLogicSeqCheck(false, 100, 7) == LANE_SEQ_TAKE);
    check("seq check: the expected one -> take", laneLogicSeqCheck(true, 100, 100) == LANE_SEQ_TAKE);
    check("seq check: ahead -> gap", laneLogicSeqCheck(true, 100, 103) == LANE_SEQ_GAP);
    check("seq check: the previous one again -> duplicate", laneLogicSeqCheck(true, 100, 99) == LANE_SEQ_DUPLICATE);
    check("seq check: a whole re-sent read (4 back) -> duplicate", laneLogicSeqCheck(true, 100, 96) == LANE_SEQ_DUPLICATE);
    check("seq check: duplicate across the wrap", laneLogicSeqCheck(true, 1, 65534) == LANE_SEQ_DUPLICATE);
    check("seq check: gap across the wrap", laneLogicSeqCheck(true, 65534, 2) == LANE_SEQ_GAP);
    check("seq check: window edge is still a duplicate",
          laneLogicSeqCheck(true, 1000, (uint16_t)(1000 - LANE_LOGIC_SEQ_DUP_WINDOW)) == LANE_SEQ_DUPLICATE);
    check("seq check: beyond the window -> resync",
          laneLogicSeqCheck(true, 1000, (uint16_t)(1000 - LANE_LOGIC_SEQ_DUP_WINDOW - 1U)) == LANE_SEQ_RESYNC);
    check("seq check: half the range ahead -> resync, not gap", laneLogicSeqCheck(true, 0, 0x8000) == LANE_SEQ_RESYNC);
}

static void testHealth(void)
{
    laneHealthView_t v = { 0 };
    laneStatus_t st = { .state = LANE_STATE_SAMPLING, .flags = LANE_STATUS_CONFIG_VALID | LANE_STATUS_SYNC_ALIGNED };
    uint8_t out[3];
    check("health: first healthy STATUS publishes nothing", laneLogicHealth(&v, &st, out) == 0 && v.known);
    st.flags |= LANE_STATUS_SYNC_LOST;
    check("health: SYNC_LOST on the rising flag", laneLogicHealth(&v, &st, out) == 1 && out[0] == HP_LANE_STATE_SYNC_LOST);
    check("health: ... once", laneLogicHealth(&v, &st, out) == 0);
    st.flags &= (uint16_t)~LANE_STATUS_SYNC_LOST;
    check("health: SYNC_OK when it clears", laneLogicHealth(&v, &st, out) == 1 && out[0] == HP_LANE_STATE_SYNC_OK);
    st.lastError = -5;
    check("health: FAULT on a new lastError", laneLogicHealth(&v, &st, out) == 1 && out[0] == HP_LANE_STATE_FAULT);
    check("health: ... once", laneLogicHealth(&v, &st, out) == 0);
    st.lastError = -7;
    check("health: FAULT again on another error", laneLogicHealth(&v, &st, out) == 1);
    st.lastError = 0;
    st.state = LANE_STATE_PARKED;
    laneHealthView_t v2 = { 0 };
    check("health: PARKED is a fault on the first STATUS",
          laneLogicHealth(&v2, &st, out) == 1 && out[0] == HP_LANE_STATE_FAULT);
}

static void testIdleSafe(void)
{
    laneStatus_t st;
    memset(&st, 0, sizeof(st));
    st.state = LANE_STATE_IDLE;
    st.flags = LANE_STATUS_CONFIG_VALID | LANE_STATUS_SYNC_ALIGNED;
    check("idle-safe: flag clear -> no mode push", !laneLogicNeedsModePush(&st));
    st.flags |= LANE_STATUS_BUS_IDLE_SAFE;
    check("idle-safe: BUS_IDLE_SAFE -> re-push the mode", laneLogicNeedsModePush(&st));
    st.flags = LANE_STATUS_BUS_IDLE_SAFE;
    check("idle-safe: other flags do not matter", laneLogicNeedsModePush(&st));
}

static void testConfigured(void)
{
    laneStatus_t st = { .state = LANE_STATE_IDLE, .flags = LANE_STATUS_CONFIG_VALID | LANE_STATUS_SYNC_ALIGNED };
    check("configured: IDLE + config + aligned", laneLogicIsConfigured(&st) && !laneLogicNeedsPush(&st));
    st.flags = LANE_STATUS_CONFIG_VALID;
    check("configured: not aligned -> no", !laneLogicIsConfigured(&st));
    st.flags = LANE_STATUS_SYNC_ALIGNED;
    check("configured: no config -> no, needs a push", !laneLogicIsConfigured(&st) && laneLogicNeedsPush(&st));
    st.flags = LANE_STATUS_CONFIG_VALID | LANE_STATUS_SYNC_ALIGNED;
    st.state = LANE_STATE_ENUMERATED;
    check("configured: still ENUMERATED -> no, needs a push", !laneLogicIsConfigured(&st) && laneLogicNeedsPush(&st));
}

static void testConfigCheck(void)
{
    hpLaneConfig_t c;
    laneLogicDefaultConfig(1, &c);
    check("config: defaults pass", laneLogicCheckConfig(&c) == CTRL_OK);
    hpLaneConfig_t d;
    laneLogicDefaultConfig(6, &d);
    check("config: lane 6 phase staggered by 5/6 period", d.samplePhaseUs == 830U && laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.samplePeriodUs = 0;
    check("config: period 0 refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.samplePhaseUs = d.samplePeriodUs;
    check("config: phase not inside the period refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.rawDecimation = 0;
    check("config: decimation 0 refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.rawDecimation = LANE_CFG_RAW_DECIMATION_MAX;
    check("config: decimation at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.rawDecimation = LANE_CFG_RAW_DECIMATION_MAX + 1U;
    check("config: decimation above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.thrDropPermille = LANE_CFG_THR_DROP_PERMILLE_MAX;
    check("config: threshold at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.thrDropPermille = LANE_CFG_THR_DROP_PERMILLE_MAX + 1U;
    check("config: threshold above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.hystPermille = d.thrDropPermille;
    check("config: hysteresis not below the drop refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.samplePeriodUs = LANE_CFG_SAMPLE_PERIOD_US_MAX;
    check("config: period at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.samplePeriodUs = LANE_CFG_SAMPLE_PERIOD_US_MAX + 1U;
    check("config: period above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.mergeGapUs = LANE_CFG_MERGE_GAP_US_MAX; d.blockTimeoutUs = LANE_CFG_MERGE_GAP_US_MAX + 1U;
    check("config: merge gap at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.mergeGapUs = LANE_CFG_MERGE_GAP_US_MAX + 1U; d.blockTimeoutUs = 2000000U;
    check("config: merge gap above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.blockTimeoutUs = d.mergeGapUs;
    check("config: block timeout not above the merge gap refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.blockTimeoutUs = LANE_CFG_BLOCK_TIMEOUT_US_MAX;
    check("config: block timeout at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.blockTimeoutUs = LANE_CFG_BLOCK_TIMEOUT_US_MAX + 1U;
    check("config: block timeout above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.holdoffUs = LANE_CFG_HOLDOFF_US_MAX;
    check("config: holdoff at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.holdoffUs = LANE_CFG_HOLDOFF_US_MAX + 1U;
    check("config: holdoff above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.holdoffSamplePeriodUs = d.samplePeriodUs;
    check("config: holdoff period = period passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.holdoffSamplePeriodUs = d.samplePeriodUs - 1U;
    check("config: holdoff period below the period refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.holdoffSamplePeriodUs = LANE_CFG_HOLDOFF_SAMPLE_PERIOD_US_MAX;
    check("config: holdoff period at max passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.holdoffSamplePeriodUs = LANE_CFG_HOLDOFF_SAMPLE_PERIOD_US_MAX + 1U;
    check("config: holdoff period above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.minDwell10us = (uint16_t)(d.blockTimeoutUs / 10U);
    check("config: min dwell not below the block timeout refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.minDwell10us = (uint16_t)(d.blockTimeoutUs / 10U - 1U);
    check("config: min dwell just below the block timeout passes", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.irCurrent = LANE_CFG_IR_CURRENT_MAX;
    d.psIntegration = LANE_CFG_PS_INTEGRATION_MAX; d.psMultiPulse = LANE_CFG_PS_MULTI_PULSE_MAX;
    check("config: sensor codes at max pass", laneLogicCheckConfig(&d) == CTRL_OK);
    d = c; d.irCurrent = LANE_CFG_IR_CURRENT_MAX + 1U;
    check("config: IR current above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.psIntegration = LANE_CFG_PS_INTEGRATION_MAX + 1U;
    check("config: PS integration above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.psMultiPulse = LANE_CFG_PS_MULTI_PULSE_MAX + 1U;
    check("config: PS multi-pulse above max refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
    d = c; d.reserved = 1;
    check("config: reserved byte set refused", laneLogicCheckConfig(&d) == CTRL_ERR_INVALID_ARG);
}

static void testMapping(void)
{
    laneStatus_t st = { .state = 3, .samplingMode = 2, .flags = 0x1234, .fifoCount = 5, .droppedEvents = 6, .syncShifts = 7,
                        .badWrites = 8, .lateActions = 9, .lastError = -10, .resetCause = 2, .faultId = 4, .uptimeMs = 99 };
    laneSignal_t sig = { .raw = 1, .baseline = 2, .thrLow = 3, .thrHigh = 4, .noise = 5 };
    hpLaneStatusRsp_t r;
    laneLogicStatusRsp(4, &st, &sig, &r);
    check("status: every field mapped", r.lane == 4 && r.state == 3 && r.samplingMode == 2 && r.flags == 0x1234
          && r.fifoCount == 5 && r.droppedEvents == 6 && r.syncShifts == 7 && r.badWrites == 8 && r.lateActions == 9
          && r.lastError == -10 && r.resetCause == 2 && r.faultId == 4 && r.uptimeMs == 99 && r.raw == 1
          && r.baseline == 2 && r.thrLow == 3 && r.thrHigh == 4 && r.noise == 5);
    laneId_t id = { .boardType = 2, .protoMinor = 1, .fwMajor = 1, .fwMinor = 2, .fwPatch = 3, .fwGitRev = 0xabc, .imageCrc = 0xdead };
    hpNodeInfo_t n;
    laneLogicNodeInfo(5, &id, HP_NODE_STATE_OK, &n);
    check("node: lane entry from the ID register", n.nodeClass == HP_NODE_LANE && n.index == 5 && n.boardType == 2
          && n.state == HP_NODE_STATE_OK && n.fwMajor == 1 && n.fwPatch == 3 && n.protoMinor == 1
          && n.fwGitRev == 0xabcU && n.imageCrc == 0xdeadU);
}

static void testSeqStart(void)
{
    bool     known = false;
    uint16_t next  = 0U;
    laneStatus_t st = { .fifoCount = 3, .nextEventSeq = 10 };
    laneLogicSeqStart(&st, &known, &next);
    check("seq start: no drops -> oldest queued = next - count", known && next == 7);
    st.nextEventSeq = 1; st.fifoCount = 3;
    laneLogicSeqStart(&st, &known, &next);
    check("seq start: wraps below 0", known && next == 0xFFFEU);
    st.fifoCount = LANE_FIFO_COUNT_MAX;
    laneLogicSeqStart(&st, &known, &next);
    check("seq start: saturated count -> unknown, first record sets it", !known);
    /* FIFO held 0..2479, 10 dropped (seq 2480..2489 spent), drained to 100:
     * queued 2380..2479, next 2490. next - count = 2390 would drop 2380..2389. */
    st = (laneStatus_t){ .fifoCount = 100, .nextEventSeq = 2490, .droppedEvents = 10 };
    laneLogicSeqStart(&st, &known, &next);
    check("seq start: lane dropped events -> unknown (no false duplicates)", !known);
    check("seq start: ... and the oldest record is then taken",
          laneLogicSeqCheck(known, next, 2380) == LANE_SEQ_TAKE);
    st = (laneStatus_t){ .fifoCount = 5, .nextEventSeq = 50, .flags = LANE_STATUS_FIFO_OVERFLOW };
    laneLogicSeqStart(&st, &known, &next);
    check("seq start: overflow flag alone (drop count cleared) -> unknown", !known);
}

static void testSchedVerify(void)
{
    check("sched verify: holds what was queued -> OK", laneLogicSchedVerify(true, 5, 5, 1) == LANE_SCHED_VERIFY_OK);
    check("sched verify: holds more (older entries) -> OK", laneLogicSchedVerify(true, 6, 5, 1) == LANE_SCHED_VERIFY_OK);
    check("sched verify: lane not caught up -> retry", laneLogicSchedVerify(true, 2, 5, 1) == LANE_SCHED_VERIFY_RETRY);
    check("sched verify: still short at the last try -> FAILED",
          laneLogicSchedVerify(true, 4, 5, LANE_LOGIC_SCHED_VERIFY_TRIES) == LANE_SCHED_VERIFY_FAILED);
    check("sched verify: one before the last -> retry",
          laneLogicSchedVerify(true, 4, 5, LANE_LOGIC_SCHED_VERIFY_TRIES - 1U) == LANE_SCHED_VERIFY_RETRY);
    check("sched verify: read failed -> FAILED at once", laneLogicSchedVerify(false, 5, 5, 1) == LANE_SCHED_VERIFY_FAILED);
    check("sched verify: tries x spacing stays far inside the 150 ms lead",
          LANE_LOGIC_SCHED_VERIFY_TRIES * LANE_LOGIC_SCHED_VERIFY_RETRY_MS <= 50U);
}

int main(void)
{
    testSeqStart();
    testSchedVerify();
    testIdleSafe();
    testJoin();
    testFifo();
    testCrossing();
    testHealth();
    testConfigured();
    testConfigCheck();
    testMapping();
    return finish();
}
