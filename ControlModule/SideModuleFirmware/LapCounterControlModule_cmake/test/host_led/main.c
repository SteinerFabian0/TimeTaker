/*
 * Host test for the panel LEDs: led_pattern.c (gamma curve, blink clock) and
 * indicator_led.c (the TIM8_CH2 / TIM1_CH1 driver) against fake timers.
 *
 * The fake models the C5 HAL: it refuses (and on the target traps on) starting
 * a running channel or stopping an idle one, can be told to fail a call, and
 * tracks MOE like the advanced-timer HAL does. The driver must only believe a
 * switch the HAL accepted, so a failed call is retried rather than lost.
 * Ported from the lane tree's host_led, extended to two instances.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32_hal.h"
#include "mx_tim1.h"
#include "mx_tim8.h"
#include "led_pattern.h"
#include "indicator_led.h"
#include "../test_check.h"

uint32_t hostTickMs;

struct hostTimHandle {
    uint32_t          period;       /* ARR */
    hal_tim_channel_t ch;           /* the one channel this LED uses */
    uint32_t          pulse;        /* CCRx */
    bool              channelOn;    /* CCxE */
    bool              counterOn;    /* CEN */
    int               failNext;     /* fail this many upcoming channel start/stop calls */
    int               failCounter;  /* fail the next counter start/stop only */
    int               misuse;       /* start of a running / stop of an idle unit */
};

static struct hostTimHandle tim1;
static struct hostTimHandle tim8;

static void resetTimers(void)
{
    tim1 = (struct hostTimHandle){ .period = 999U, .ch = HAL_TIM_CHANNEL_1 };
    tim8 = (struct hostTimHandle){ .period = 999U, .ch = HAL_TIM_CHANNEL_2 };
}

hal_tim_handle_t *mx_tim1_gethandle(void) { return &tim1; }
hal_tim_handle_t *mx_tim8_gethandle(void) { return &tim8; }

uint32_t HAL_TIM_GetPeriod(const hal_tim_handle_t *htim)
{
    return htim->period;
}

hal_status_t HAL_TIM_OC_SetCompareUnitPulse(hal_tim_handle_t *htim,
                                            hal_tim_oc_compare_unit_t compare_unit,
                                            uint32_t pulse)
{
    if (compare_unit != hal_tim_oc_channel_to_compare_unit(htim->ch)) {
        return HAL_INVALID_PARAM;
    }
    htim->pulse = pulse;
    return HAL_OK;
}

static hal_status_t fakeSwitch(hal_tim_handle_t *htim, bool *unit, bool on)
{
    if (*unit == on) {
        htim->misuse++;
        return HAL_ERROR;
    }
    *unit = on;
    return HAL_OK;
}

hal_status_t HAL_TIM_OC_StartChannel(hal_tim_handle_t *htim, hal_tim_channel_t channel)
{
    if (channel != htim->ch) {
        return HAL_INVALID_PARAM;
    }
    if (htim->failNext > 0) {
        htim->failNext--;
        return HAL_ERROR;
    }
    return fakeSwitch(htim, &htim->channelOn, true);
}

hal_status_t HAL_TIM_OC_StopChannel(hal_tim_handle_t *htim, hal_tim_channel_t channel)
{
    if (channel != htim->ch) {
        return HAL_INVALID_PARAM;
    }
    if (htim->failNext > 0) {
        htim->failNext--;
        return HAL_ERROR;
    }
    return fakeSwitch(htim, &htim->channelOn, false);
}

hal_status_t HAL_TIM_Start(hal_tim_handle_t *htim)
{
    if (htim->failCounter > 0) {
        htim->failCounter--;
        return HAL_ERROR;
    }
    return fakeSwitch(htim, &htim->counterOn, true);
}

hal_status_t HAL_TIM_Stop(hal_tim_handle_t *htim)
{
    if (htim->failCounter > 0) {
        htim->failCounter--;
        return HAL_ERROR;
    }
    return fakeSwitch(htim, &htim->counterOn, false);
}

static bool pinLit(const struct hostTimHandle *t)
{
    return t->channelOn && t->counterOn && t->pulse > 0U;
}


/* --- Gamma ------------------------------------------------------------- */

static void testGamma(void)
{
    const uint32_t period = 1000U;

    check("gamma: level 0 is dark",           ledPatternDutyCounts(0U, period) == 0U);
    check("gamma: max level is full duty",    ledPatternDutyCounts(LED_LEVEL_MAX, period) == period);
    check("gamma: above max clamps",          ledPatternDutyCounts(LED_LEVEL_MAX + 500U, period) == period);
    check("gamma: level 1 still lights",      ledPatternDutyCounts(1U, period) >= 1U);

    int      monotonic = 1;
    uint32_t prev      = 0U;
    for (uint16_t level = 0U; level <= LED_LEVEL_MAX; level++) {
        uint32_t d = ledPatternDutyCounts(level, period);
        monotonic  = monotonic && d >= prev;
        prev       = d;
    }
    check("gamma: monotonic over all levels", monotonic);

    /* Gamma 2.2 at the midpoint is ~21.8 %, not 50 %. */
    uint32_t mid = ledPatternDutyCounts(LED_LEVEL_MAX / 2U, period);
    check("gamma: midpoint ~21.8 % duty",     mid >= 210U && mid <= 225U);

    check("gamma: scales with the period",
          ledPatternDutyCounts(LED_LEVEL_MAX / 2U, 65536U) >= 14000U
          && ledPatternDutyCounts(LED_LEVEL_MAX / 2U, 65536U) <= 14800U);
    check("gamma: no overflow at a full 16-bit period",
          ledPatternDutyCounts(LED_LEVEL_MAX - 1U, 65536U) < 65536U);
}


/* --- Blink clock --------------------------------------------------------- */

static void testBlinkParams(void)
{
    ledBlink_t b = { 0 };

    check("blink: valid start",               ledBlinkStart(&b, 0U, 100U, 500U, 3U) == CTRL_OK);
    b.active = false;
    check("blink: period below minimum",      ledBlinkStart(&b, 0U, LED_BLINK_PERIOD_MIN_MS - 1U, 500U, 3U) == CTRL_ERR_INVALID_ARG);
    check("blink: period above maximum",      ledBlinkStart(&b, 0U, LED_BLINK_PERIOD_MAX_MS + 1U, 500U, 3U) == CTRL_ERR_INVALID_ARG);
    check("blink: on share 0",                ledBlinkStart(&b, 0U, 100U, 0U, 3U) == CTRL_ERR_INVALID_ARG);
    check("blink: on share 1000",             ledBlinkStart(&b, 0U, 100U, 1000U, 3U) == CTRL_ERR_INVALID_ARG);
    check("blink: on phase rounds to 0 ms",   ledBlinkStart(&b, 0U, 20U, 10U, 3U) == CTRL_ERR_INVALID_ARG);
    check("blink: rejected start leaves it idle", !b.active);
    check("blink: NULL",                      ledBlinkStart(NULL, 0U, 100U, 500U, 3U) == CTRL_ERR_INVALID_ARG);
}

static void testBlinkClock(void)
{
    ledBlink_t b;

    (void)ledBlinkStart(&b, 1000U, 100U, 300U, 2U);
    check("blink: lit at start",              ledBlinkIsOn(&b, 1000U));
    check("blink: lit until on time",         ledBlinkIsOn(&b, 1029U));
    check("blink: dark after on time",        !ledBlinkIsOn(&b, 1030U));
    check("blink: second flash",              ledBlinkIsOn(&b, 1100U));
    check("blink: finite blink ends dark",    !ledBlinkIsOn(&b, 1200U) && !b.active);
    check("blink: stays dark once done",      !ledBlinkIsOn(&b, 1300U));

    (void)ledBlinkStart(&b, 0xFFFFFFF0U, 100U, 500U, 0U);
    check("blink: endless, across ms wrap",   ledBlinkIsOn(&b, 0x00000020U) && !ledBlinkIsOn(&b, 0x00000050U));
    check("blink: endless keeps going",       b.active);
    ledBlinkStop(&b);
    check("blink: stop",                      !ledBlinkIsOn(&b, 0U));

    /* The panel's real patterns. */
    (void)ledBlinkStart(&b, 0U, 2000U, 50U, 0U);
    check("pattern no host: 100 ms blip then dark until 2 s",
          ledBlinkIsOn(&b, 99U) && !ledBlinkIsOn(&b, 100U) && !ledBlinkIsOn(&b, 1999U) && ledBlinkIsOn(&b, 2000U));
    (void)ledBlinkStart(&b, 0U, 1000U, 500U, 0U);
    check("pattern no session: 1 Hz, 50 %",
          ledBlinkIsOn(&b, 499U) && !ledBlinkIsOn(&b, 500U) && ledBlinkIsOn(&b, 1000U));
    (void)ledBlinkStart(&b, 0U, 60U, 500U, 1U);
    check("pattern traffic: one 30 ms flash, then dark for good",
          ledBlinkIsOn(&b, 29U) && !ledBlinkIsOn(&b, 30U) && !ledBlinkIsOn(&b, 60U) && !b.active);
}


/* --- Driver --------------------------------------------------------------- */

static void testDriver(indicatorLed_e led, struct hostTimHandle *t, const char *tag)
{
    char name[96];
#define CHK(text, cond) do { snprintf(name, sizeof(name), "%s: %s", tag, text); check(name, (cond)); } while (0)

    resetTimers();
    hostTickMs = 0U;

    CHK("init ok",                       indicatorLedInit() == CTRL_OK);
    CHK("init: dark, default level programmed",
        !pinLit(t) && t->pulse == ledPatternDutyCounts(INDICATOR_LED_DEFAULT_LEVEL, 1000U));

    CHK("on",                            indicatorLedSet(led, true) == CTRL_OK && pinLit(t));
    CHK("on again: no HAL misuse",       indicatorLedSet(led, true) == CTRL_OK && t->misuse == 0);
    CHK("level while lit",               indicatorLedSetLevel(led, 500U) == CTRL_OK
                                         && t->pulse == ledPatternDutyCounts(500U, 1000U));
    CHK("off parks dark",                indicatorLedSet(led, false) == CTRL_OK && !t->channelOn && !t->counterOn);
    CHK("off again: no HAL misuse",      indicatorLedSet(led, false) == CTRL_OK && t->misuse == 0);
    CHK("level while dark is kept",      indicatorLedSetLevel(led, 100U) == CTRL_OK && !pinLit(t));
    CHK("on uses the kept level",        indicatorLedSet(led, true) == CTRL_OK
                                         && t->pulse == ledPatternDutyCounts(100U, 1000U));
    CHK("level clamps",                  indicatorLedSetLevel(led, 5000U) == CTRL_OK && t->pulse == 1000U);

    t->failNext = 1;
    CHK("off: HAL failure reported",     indicatorLedSet(led, false) == CTRL_ERR_TIMER);
    CHK("off: state not believed",       indicatorLedIsLit(led));
    CHK("off: retry succeeds",           indicatorLedSet(led, false) == CTRL_OK && !t->channelOn && !t->counterOn);

    t->failNext = 1;
    CHK("on: channel refused, reported", indicatorLedSet(led, true) == CTRL_ERR_TIMER && !indicatorLedIsLit(led));
    CHK("on: retry reaches lit",         indicatorLedSet(led, true) == CTRL_OK && pinLit(t));
    (void)indicatorLedSet(led, false);

    /* Halfway: channel started, counter refused. The retry must finish the
     * switch, not re-start the running channel (the HAL traps on that). */
    t->failCounter = 1;
    CHK("on: counter refused, reported", indicatorLedSet(led, true) == CTRL_ERR_TIMER
                                         && t->channelOn && !indicatorLedIsLit(led));
    CHK("on: halfway retry completes",   indicatorLedSet(led, true) == CTRL_OK && pinLit(t) && t->misuse == 0);

    t->failCounter = 1;
    CHK("off: counter refused, pin already dark",
        indicatorLedSet(led, false) == CTRL_ERR_TIMER && !t->channelOn && !pinLit(t));
    CHK("off: halfway retry completes",  indicatorLedSet(led, false) == CTRL_OK && !t->counterOn && t->misuse == 0);
    CHK("no HAL misuse so far",          t->misuse == 0);
#undef CHK
}

static void testDriverBlink(indicatorLed_e led, struct hostTimHandle *t, const char *tag)
{
    char name[96];
#define CHK(text, cond) do { snprintf(name, sizeof(name), "%s: %s", tag, text); check(name, (cond)); } while (0)

    resetTimers();
    hostTickMs = 500U;
    (void)indicatorLedInit();

    CHK("blink starts lit",              indicatorLedBlink(led, 100U, 500U, 2U) == CTRL_OK && pinLit(t));
    hostTickMs = 560U;
    CHK("blink dark phase",              indicatorLedRun(hostTickMs) == CTRL_OK && !pinLit(t));
    hostTickMs = 600U;
    CHK("blink second flash",            indicatorLedRun(hostTickMs) == CTRL_OK && pinLit(t));
    hostTickMs = 700U;
    CHK("blink ends dark",               indicatorLedRun(hostTickMs) == CTRL_OK && !pinLit(t) && !indicatorLedIsBlinking(led));
    CHK("blink bad params refused",      indicatorLedBlink(led, 5U, 500U, 2U) == CTRL_ERR_INVALID_ARG
                                         && !indicatorLedIsBlinking(led));

    /* The final dark switch of a finite blink fails: it must be retried even
     * though the blink is no longer active, or the LED stays lit. */
    hostTickMs = 1000U;
    (void)indicatorLedBlink(led, 100U, 500U, 1U);
    hostTickMs = 1100U;
    t->failNext = 1;
    CHK("finished blink: dark switch fails, reported", indicatorLedRun(hostTickMs) == CTRL_ERR_TIMER && t->channelOn);
    hostTickMs = 1110U;
    CHK("finished blink: retried and dark", indicatorLedRun(hostTickMs) == CTRL_OK && !t->channelOn && !t->counterOn);
    CHK("finished blink: settled, no more calls", indicatorLedRun(1120U) == CTRL_OK && t->misuse == 0);

    /* A new blink replacing a running one, and a steady set replacing a blink. */
    hostTickMs = 2000U;
    (void)indicatorLedBlink(led, 2000U, 50U, 0U);
    hostTickMs = 2500U;
    (void)indicatorLedRun(hostTickMs);
    (void)indicatorLedBlink(led, 1000U, 500U, 0U);
    CHK("replaced blink restarts lit",   pinLit(t) && indicatorLedIsBlinking(led));
    (void)indicatorLedSet(led, true);
    CHK("set cancels blink, stays lit",  !indicatorLedIsBlinking(led) && pinLit(t));
    hostTickMs = 3700U;
    CHK("stays lit after cancel",        indicatorLedRun(hostTickMs) == CTRL_OK && pinLit(t));
    (void)indicatorLedSet(led, false);
    CHK("no HAL misuse",                 t->misuse == 0);
#undef CHK
}

static void testIndependent(void)
{
    resetTimers();
    (void)indicatorLedInit();
    (void)indicatorLedSet(INDICATOR_LED_STATUS, true);
    check("LEDs are independent: STATUS on leaves ACT dark", pinLit(&tim8) && !tim1.channelOn);
    check("invalid LED refused", indicatorLedSet(INDICATOR_LED_COUNT, true) == CTRL_ERR_INVALID_ARG
                                 && indicatorLedSetLevel(INDICATOR_LED_COUNT, 1U) == CTRL_ERR_INVALID_ARG
                                 && indicatorLedBlink(INDICATOR_LED_COUNT, 100U, 500U, 1U) == CTRL_ERR_INVALID_ARG);
}

int main(void)
{
    testGamma();
    testBlinkParams();
    testBlinkClock();
    testDriver(INDICATOR_LED_STATUS, &tim8, "STATUS/TIM8");
    testDriver(INDICATOR_LED_ACT, &tim1, "ACT/TIM1");
    testDriverBlink(INDICATOR_LED_STATUS, &tim8, "STATUS/TIM8");
    testDriverBlink(INDICATOR_LED_ACT, &tim1, "ACT/TIM1");
    testIndependent();
    return finish();
}
