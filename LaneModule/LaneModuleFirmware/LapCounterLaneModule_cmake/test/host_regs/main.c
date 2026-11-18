/*
 * Host test for lane_regs.c (register-map decoding and validation) and
 * image_crc.c (the ID register's CRC32).
 *
 * Every rejection case starts from a valid frame and breaks exactly one
 * rule, so deleting that rule from lane_regs.c fails the case. Each
 * rejection must also count in badWrites.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lane_regs.h"
#include "image_crc.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-58s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

static laneRegs_t      regs;
static laneRegsWrite_t w;
static uint8_t         frame[64];

/* Frame = [reg][payload]. */
static uint32_t mkFrame(uint8_t reg, const void *payload, uint32_t size)
{
    frame[0] = reg;
    if (size > 0U) {
        memcpy(&frame[1], payload, size);
    }
    return 1U + size;
}

/* Decodes and checks the expected status and that a rejection was counted. */
static void expect(const char *name, uint8_t reg, const void *payload, uint32_t size,
                   laneStatus_e want)
{
    uint16_t before = laneRegsBadWrites(&regs);
    laneStatus_e got = laneRegsDecode(&regs, frame, mkFrame(reg, payload, size), &w);
    uint16_t after   = laneRegsBadWrites(&regs);
    bool counted     = (want == LANE_OK) ? (after == before) : (after == before + 1U);
    check(name, got == want && counted);
}


/* --- Frames and lengths --------------------------------------------------- */

static void testFraming(void)
{
    laneRegsInit(&regs);

    for (uint8_t r = 0U; r <= (uint8_t)LANE_REG_RAW_FIFO; r++) {
        if (laneRegsDecode(&regs, frame, mkFrame(r, NULL, 0U), &w) != LANE_OK) {
            check("pointer-only write valid for every register", false);
        }
    }
    check("pointer-only write valid for every register", laneRegsBadWrites(&regs) == 0U);
    check("pointer-only: payloadLen 0", w.payloadLen == 0U);

    expect("unknown register 0x0B rejected", 0x0BU, NULL, 0U, LANE_ERR_REG_UNKNOWN);
    expect("unknown register 0xFF rejected", 0xFFU, NULL, 0U, LANE_ERR_REG_UNKNOWN);

    uint8_t junk[28] = { 0 };
    expect("payload to ID rejected (read-only)",         LANE_REG_ID,         junk, 28U, LANE_ERR_REG_READ_ONLY);
    expect("payload to STATUS rejected (read-only)",     LANE_REG_STATUS,     junk, 24U, LANE_ERR_REG_READ_ONLY);
    expect("payload to EVENT_FIFO rejected (read-only)", LANE_REG_EVENT_FIFO, junk, 4U,  LANE_ERR_REG_READ_ONLY);
    expect("payload to SIGNAL rejected (read-only)",     LANE_REG_SIGNAL,     junk, 20U, LANE_ERR_REG_READ_ONLY);
    expect("payload to RAW_FIFO rejected (read-only)",   LANE_REG_RAW_FIFO,   junk, 16U, LANE_ERR_REG_READ_ONLY);

    laneConfig_t c;
    laneRegsDefaultConfig(&c);
    expect("CONFIG one byte short rejected", LANE_REG_CONFIG, &c, sizeof(c) - 1U, LANE_ERR_REG_LENGTH);
    uint8_t longer[sizeof(laneConfig_t) + 1U] = { 0 };
    memcpy(longer, &c, sizeof(c));
    expect("CONFIG one byte long rejected", LANE_REG_CONFIG, longer, sizeof(longer), LANE_ERR_REG_LENGTH);
    laneIndex_t li = { 3U };
    expect("LANE_ID right length accepted", LANE_REG_LANE_ID, &li, 1U, LANE_OK);
    uint8_t two[2] = { 3U, 0U };
    expect("LANE_ID two bytes rejected", LANE_REG_LANE_ID, two, 2U, LANE_ERR_REG_LENGTH);
    laneLed_t led;
    memset(&led, 0, sizeof(led));
    expect("LED short rejected", LANE_REG_LED, &led, sizeof(led) - 2U, LANE_ERR_REG_LENGTH);
    laneSchedule_t s;
    memset(&s, 0, sizeof(s));
    s.op = LANE_SCHED_CANCEL_ALL;
    expect("SCHEDULE short rejected", LANE_REG_SCHEDULE, &s, sizeof(s) - 8U, LANE_ERR_REG_LENGTH);
    laneCommand_t cmd = { LANE_CMD_CALIBRATE, 0U, 0U };
    expect("COMMAND short rejected", LANE_REG_COMMAND, &cmd, 2U, LANE_ERR_REG_LENGTH);
    laneSyncAlign_t sa = { 5U, 10U, 1000U };
    expect("SYNC_ALIGN short rejected", LANE_REG_SYNC_ALIGN, &sa, 8U, LANE_ERR_REG_LENGTH);

    uint16_t before = laneRegsBadWrites(&regs);
    check("zero-length frame ignored, not counted",
          laneRegsDecode(&regs, frame, 0U, &w) == LANE_ERR_REG_LENGTH && laneRegsBadWrites(&regs) == before);
    check("last reject remembered", regs.lastReject == LANE_ERR_REG_LENGTH);
}


/* --- CONFIG --------------------------------------------------------------- */

static void testConfig(void)
{
    laneConfig_t good;
    laneConfig_t c;
    laneRegsInit(&regs);
    laneRegsDefaultConfig(&good);

    expect("CONFIG defaults accepted", LANE_REG_CONFIG, &good, sizeof(good), LANE_OK);
    check("CONFIG decoded whole (atomic copy)",
          w.payloadLen == sizeof(laneConfig_t) && memcmp(&w.u.config, &good, sizeof(good)) == 0);
    check("CONFIG defaults = control module golden",
          good.samplePeriodUs == 1000U && good.mergeGapUs == 30000U && good.blockTimeoutUs == 500000U
          && good.thrDropPermille == 300U && good.hystPermille == 100U && good.minDwell10us == 200U
          && good.rawDecimation == 1U && good.irCurrent == 7U && good.psIntegration == 3U
          && good.psMultiPulse == 0U);

    c = good; c.psIntegration = 0U; c.samplePeriodUs = laneRegsMinSamplePeriodUs(0U, 0U) - 1U;
    expect("CONFIG period below the fastest sensor cycle", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c.samplePeriodUs++;
    expect("CONFIG period = fastest sensor cycle accepted", LANE_REG_CONFIG, &c, sizeof(c), LANE_OK);
    c = good; c.samplePeriodUs = LANE_CFG_SAMPLE_PERIOD_US_MAX + 1U;
    expect("CONFIG period above maximum", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.samplePeriodUs = LANE_CFG_SAMPLE_PERIOD_US_MAX;
    expect("CONFIG period at maximum accepted", LANE_REG_CONFIG, &c, sizeof(c), LANE_OK);
    c = good; c.psIntegration = 7U; c.psMultiPulse = 3U;
    expect("CONFIG period shorter than the conversion (8T x8)", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c.samplePeriodUs = laneRegsMinSamplePeriodUs(7U, 3U);
    expect("CONFIG same codes with a long enough period", LANE_REG_CONFIG, &c, sizeof(c), LANE_OK);
    c = good; c.samplePhaseUs = c.samplePeriodUs;
    expect("CONFIG phase = period", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.samplePhaseUs = c.samplePeriodUs - 1U;
    expect("CONFIG phase = period - 1 accepted", LANE_REG_CONFIG, &c, sizeof(c), LANE_OK);
    c = good; c.mergeGapUs = LANE_CFG_MERGE_GAP_US_MAX + 1U; c.blockTimeoutUs = 2000000U;
    expect("CONFIG merge gap above maximum", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.blockTimeoutUs = c.mergeGapUs;
    expect("CONFIG block timeout = merge gap", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.blockTimeoutUs = LANE_CFG_BLOCK_TIMEOUT_US_MAX + 1U;
    expect("CONFIG block timeout above maximum", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.holdoffUs = LANE_CFG_HOLDOFF_US_MAX + 1U;
    expect("CONFIG holdoff above 10 min", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.holdoffUs = 30000000U; c.holdoffSamplePeriodUs = 10000U;
    expect("CONFIG holdoff 30 s at 100 Hz accepted", LANE_REG_CONFIG, &c, sizeof(c), LANE_OK);
    c = good; c.holdoffSamplePeriodUs = c.samplePeriodUs - 1U;
    expect("CONFIG holdoff period faster than the period", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.holdoffSamplePeriodUs = LANE_CFG_HOLDOFF_SAMPLE_PERIOD_US_MAX + 1U;
    expect("CONFIG holdoff period above maximum", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.thrDropPermille = 0U; c.hystPermille = 0U;
    expect("CONFIG drop 0", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.thrDropPermille = 1000U;
    expect("CONFIG drop 1000", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.hystPermille = c.thrDropPermille;
    expect("CONFIG hysteresis = drop", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.minDwell10us = (uint16_t)(c.blockTimeoutUs / 10U);
    c.blockTimeoutUs = 600000U; c.minDwell10us = 60000U;
    expect("CONFIG min dwell = block timeout", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.rawDecimation = 0U;
    expect("CONFIG raw decimation 0", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.rawDecimation = LANE_CFG_RAW_DECIMATION_MAX + 1U;
    expect("CONFIG raw decimation above maximum", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.irCurrent = LANE_CFG_IR_CURRENT_MAX + 1U;
    expect("CONFIG IR current code 8", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.psIntegration = LANE_CFG_PS_INTEGRATION_MAX + 1U; c.samplePeriodUs = 100000U;
    expect("CONFIG PS_IT code 8", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.psMultiPulse = LANE_CFG_PS_MULTI_PULSE_MAX + 1U; c.samplePeriodUs = 100000U;
    expect("CONFIG PS_MPS code 4", LANE_REG_CONFIG, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = good; c.reserved = 0xA5U;
    expect("CONFIG reserved byte ignored", LANE_REG_CONFIG, &c, sizeof(c), LANE_OK);

    check("conversion: default codes fit 1 kHz",
          laneRegsMinSamplePeriodUs(good.psIntegration, good.psMultiPulse) <= 1000U);
    check("conversion: grows with integration and pulses",
          laneRegsConversionUs(7U, 3U) > laneRegsConversionUs(7U, 0U)
          && laneRegsConversionUs(7U, 0U) > laneRegsConversionUs(0U, 0U));
}


/* --- LED, SCHEDULE, COMMAND, SYNC_ALIGN, LANE_ID --------------------------- */

static void testLed(void)
{
    laneLed_t good;
    laneLed_t l;
    laneRegsInit(&regs);
    memset(&good, 0, sizeof(good));
    good.mode = LANE_LED_BLINK; good.level = 1000U; good.blinkPeriodMs = 500U;
    good.blinkOnPermille = 500U; good.blinkCount = 3U;

    expect("LED blink accepted", LANE_REG_LED, &good, sizeof(good), LANE_OK);
    l = good; l.mode = 3U;
    expect("LED mode 3", LANE_REG_LED, &l, sizeof(l), LANE_ERR_REG_VALUE);
    l = good; l.level = 1001U;
    expect("LED level 1001", LANE_REG_LED, &l, sizeof(l), LANE_ERR_REG_VALUE);
    l = good; l.blinkPeriodMs = 19U;
    expect("LED blink period below 20 ms", LANE_REG_LED, &l, sizeof(l), LANE_ERR_REG_VALUE);
    l = good; l.blinkPeriodMs = 60001U;
    expect("LED blink period above 60 s", LANE_REG_LED, &l, sizeof(l), LANE_ERR_REG_VALUE);
    l = good; l.blinkOnPermille = 0U;
    expect("LED blink on-share 0", LANE_REG_LED, &l, sizeof(l), LANE_ERR_REG_VALUE);
    l = good; l.blinkOnPermille = 1000U;
    expect("LED blink on-share 1000", LANE_REG_LED, &l, sizeof(l), LANE_ERR_REG_VALUE);
    l = good; l.mode = LANE_LED_ON; l.blinkPeriodMs = 0U; l.blinkOnPermille = 0U;
    expect("LED ON ignores the blink fields", LANE_REG_LED, &l, sizeof(l), LANE_OK);
}

static void testSchedule(void)
{
    laneSchedule_t good;
    laneSchedule_t s;
    laneRegsInit(&regs);
    memset(&good, 0, sizeof(good));
    good.op = LANE_SCHED_ADD; good.action = LANE_ACT_LED_ON; good.param = 1000U; good.atUs = 123456789ULL;

    expect("SCHEDULE add LED_ON accepted", LANE_REG_SCHEDULE, &good, sizeof(good), LANE_OK);
    check("SCHEDULE 64-bit time decoded", w.u.schedule.atUs == 123456789ULL);
    s = good; s.op = 0U;
    expect("SCHEDULE op 0", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.op = 3U;
    expect("SCHEDULE op 3", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = 0U;
    expect("SCHEDULE action 0", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = 7U;
    expect("SCHEDULE action 7", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_PULSE; s.param = 0U;
    expect("SCHEDULE LED_PULSE rise 0 ms", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_PULSE; s.param = 19U;
    expect("SCHEDULE LED_PULSE rise 19 ms", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_PULSE; s.param = 60001U;
    expect("SCHEDULE LED_PULSE rise 60001 ms", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_PULSE; s.param = 1000U;
    expect("SCHEDULE LED_PULSE 1000 ms accepted", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_OK);
    check("SCHEDULE LED_PULSE decoded", w.u.schedule.action == LANE_ACT_LED_PULSE && w.u.schedule.param == 1000U);
    s = good; s.action = LANE_ACT_LED_PULSE; s.param = 20U;
    expect("SCHEDULE LED_PULSE 20 ms accepted", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_OK);
    s = good; s.action = LANE_ACT_LED_PULSE; s.param = 60000U;
    expect("SCHEDULE LED_PULSE 60000 ms accepted", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_OK);
    s = good; s.param = 1001U;
    expect("SCHEDULE LED_ON level 1001", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_BLINK; s.param = 19U;
    expect("SCHEDULE LED_BLINK period 19 ms", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_BLINK; s.param = 60001U;
    expect("SCHEDULE LED_BLINK period 60001 ms", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = good; s.action = LANE_ACT_LED_BLINK; s.param = 250U;
    expect("SCHEDULE LED_BLINK 250 ms accepted", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_OK);
    s = good; s.action = LANE_ACT_SAMPLING_ON; s.param = 0xFFFFU;
    expect("SCHEDULE SAMPLING_ON ignores param", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_OK);
    s = good; s.op = LANE_SCHED_CANCEL_ALL; s.action = 0xEEU;
    expect("SCHEDULE cancel-all ignores action", LANE_REG_SCHEDULE, &s, sizeof(s), LANE_OK);
}

static void testCommand(void)
{
    laneCommand_t c;
    laneRegsInit(&regs);

    c = (laneCommand_t){ LANE_CMD_SAMPLING, LANE_SAMPLING_RAW, 0U };
    expect("COMMAND sampling RAW accepted", LANE_REG_COMMAND, &c, sizeof(c), LANE_OK);
    c.arg = 3U;
    expect("COMMAND sampling mode 3", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = (laneCommand_t){ LANE_CMD_SLEEP, LANE_SLEEP_IDLE, 0U };
    expect("COMMAND sleep IDLE accepted", LANE_REG_COMMAND, &c, sizeof(c), LANE_OK);
    c.arg = LANE_SLEEP_NAP;
    expect("COMMAND sleep NAP not implemented", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_CMD_UNSUPPORTED);
    c.arg = LANE_SLEEP_STANDBY;
    expect("COMMAND sleep STANDBY not implemented", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_CMD_UNSUPPORTED);
    c.arg = 4U;
    expect("COMMAND sleep level 4 invalid", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = (laneCommand_t){ LANE_CMD_ENTER_BOOTLOADER, 0U, 0U };
    expect("COMMAND ENTER_BOOTLOADER rejected by the APP", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_CMD_UNSUPPORTED);
    c = (laneCommand_t){ 0U, 0U, 0U };
    expect("COMMAND 0 unknown", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_REG_VALUE);
    c = (laneCommand_t){ 8U, 0U, 0U };
    expect("COMMAND 8 unknown", LANE_REG_COMMAND, &c, sizeof(c), LANE_ERR_REG_VALUE);

    static const uint8_t plain[] = { LANE_CMD_CALIBRATE, LANE_CMD_SINGLE_SAMPLE, LANE_CMD_RELEASE_CHAIN,
                                     LANE_CMD_RESET, LANE_CMD_CLEAR_COUNTERS };
    bool allOk = true;
    for (uint32_t i = 0U; i < sizeof(plain); i++) {
        c = (laneCommand_t){ plain[i], 0x55U, 0U };
        allOk = allOk && laneRegsDecode(&regs, frame, mkFrame(LANE_REG_COMMAND, &c, sizeof(c)), &w) == LANE_OK;
    }
    check("COMMAND argument-less commands accepted", allOk);
}

static void testSyncAlignAndLaneId(void)
{
    laneSyncAlign_t s;
    laneRegsInit(&regs);

    s = (laneSyncAlign_t){ 70000U, 999U, 1000U };
    expect("SYNC_ALIGN accepted", LANE_REG_SYNC_ALIGN, &s, sizeof(s), LANE_OK);
    s.phaseUs = 1000U;
    expect("SYNC_ALIGN phase = period", LANE_REG_SYNC_ALIGN, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = (laneSyncAlign_t){ 0U, 0U, 99U };
    expect("SYNC_ALIGN period below 100 us", LANE_REG_SYNC_ALIGN, &s, sizeof(s), LANE_ERR_REG_VALUE);
    s = (laneSyncAlign_t){ 0U, 0U, 100001U };
    expect("SYNC_ALIGN period above 100 ms", LANE_REG_SYNC_ALIGN, &s, sizeof(s), LANE_ERR_REG_VALUE);

    laneIndex_t li = { LANE_MAX_LANES };
    expect("LANE_ID 6 accepted", LANE_REG_LANE_ID, &li, 1U, LANE_OK);
    li.laneIndex = 0U;
    expect("LANE_ID 0 (unassign) accepted", LANE_REG_LANE_ID, &li, 1U, LANE_OK);
    li.laneIndex = LANE_MAX_LANES + 1U;
    expect("LANE_ID 7 rejected", LANE_REG_LANE_ID, &li, 1U, LANE_ERR_REG_VALUE);
}

static void testCounters(void)
{
    laneRegsInit(&regs);
    regs.badWrites = 0xFFFEU;
    laneRegsReject(&regs, LANE_ERR_SCHED_FULL);
    laneRegsReject(&regs, LANE_ERR_SCHED_FULL);
    check("badWrites saturates at 0xFFFF", laneRegsBadWrites(&regs) == 0xFFFFU);
    check("caller rejection remembered", regs.lastReject == LANE_ERR_SCHED_FULL);
    laneRegsClearCounters(&regs);
    check("clear counters", laneRegsBadWrites(&regs) == 0U);
}

/* Bit-exact model of the C5 CRC unit as image_crc.c configures it (POLYSIZE 32,
 * POL 0x04C11DB7, REV_IN byte, REV_OUT bit, INIT = rbit32(~crc), final ~):
 * shift left, MSB first. It must equal the software path that these tests
 * build, which is what the target's hardware path is built to match. */
static uint32_t rbit32(uint32_t v)
{
    uint32_t r = 0U;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | ((v >> i) & 1U);
    }
    return r;
}

static uint32_t hwModelCrc32(uint32_t crc, const uint8_t *d, uint32_t len)
{
    uint32_t dr = rbit32(~crc);
    for (uint32_t i = 0U; i < len; i++) {
        uint32_t in = 0U;
        for (int b = 0; b < 8; b++) {
            in = (in << 1) | (((uint32_t)d[i] >> b) & 1U);
        }
        dr ^= in << 24;
        for (int b = 0; b < 8; b++) {
            dr = ((dr & 0x80000000U) != 0U) ? ((dr << 1) ^ 0x04C11DB7U) : (dr << 1);
        }
    }
    return ~rbit32(dr);
}

static void testCrc(void)
{
    uint8_t buf[300];
    for (uint32_t i = 0U; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 37U + 11U);
    }
    check("crc model: \"123456789\" = 0xCBF43926", hwModelCrc32(0U, (const uint8_t *)"123456789", 9U) == 0xCBF43926U);
    check("crc model equals software", hwModelCrc32(0U, buf, sizeof(buf)) == imageCrc32(0U, buf, sizeof(buf)));
    check("crc model chunked equals software",
          hwModelCrc32(hwModelCrc32(0U, buf, 123U), buf + 123, 177U) == imageCrc32(0U, buf, sizeof(buf)));
    static const uint8_t check9[] = "123456789";
    check("crc32(\"123456789\") = 0xCBF43926", imageCrc32(0U, check9, 9U) == 0xCBF43926U);
    check("crc32 continues across chunks",
          imageCrc32(imageCrc32(0U, check9, 4U), &check9[4], 5U) == 0xCBF43926U);
    check("crc32 of nothing = 0", imageCrc32(0U, check9, 0U) == 0U);
}

int main(void)
{
    testFraming();
    testConfig();
    testLed();
    testSchedule();
    testCommand();
    testSyncAlignAndLaneId();
    testCounters();
    testCrc();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
