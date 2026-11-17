/*
 * Host test for the start-light LED: led_pattern.c (gamma curve, blink clock)
 * and indicator_led.c (the TIM1 driver) against a fake TIM1.
 *
 * The fake models what the C5 HAL does to the driver: it refuses (and on the
 * target traps on) starting a running channel or stopping an idle one, and it
 * can be told to fail the next call. The driver must only believe a switch
 * the HAL accepted, so a failed call is retried rather than lost.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "stm32_hal.h"
#include "mx_tim1.h"
#include "led_pattern.h"
#include "indicator_led.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-52s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}


/* --- Fake TIM1 ------------------------------------------------------------ */

struct hostTimHandle {
    uint32_t period;        /* ARR */
    uint32_t pulse;         /* CCR1 */
    bool     channelOn;     /* CC1E (+ MOE on TIM1) */
    bool     counterOn;     /* CEN */
    int      failNext;      /* fail this many upcoming start/stop calls */
    int      failCounter;   /* fail the next counter start/stop only */
    int      misuse;        /* start of a running / stop of an idle unit */
};

static struct hostTimHandle tim1 = { .period = 999U };

hal_tim_handle_t *mx_tim1_gethandle(void)
{
    return &tim1;
}

uint32_t HAL_TIM_GetPeriod(const hal_tim_handle_t *htim)
{
    return htim->period;
}

hal_status_t HAL_TIM_OC_SetCompareUnitPulse(hal_tim_handle_t *htim,
                                            hal_tim_oc_compare_unit_t compare_unit,
                                            uint32_t pulse)
{
    if (compare_unit != HAL_TIM_OC_COMPARE_UNIT_1) {
        return HAL_INVALID_PARAM;
    }
    htim->pulse = pulse;
    return HAL_OK;
}

static hal_status_t fakeSwitch(hal_tim_handle_t *htim, bool *unit, bool on)
{
    if (htim->failNext > 0) {
        htim->failNext--;
        return HAL_ERROR;
    }
    if (*unit == on) {
        htim->misuse++;
        return HAL_ERROR;
    }
    *unit = on;
    return HAL_OK;
}

hal_status_t HAL_TIM_OC_StartChannel(hal_tim_handle_t *htim, hal_tim_channel_t channel)
{
    return (channel == HAL_TIM_CHANNEL_1) ? fakeSwitch(htim, &htim->channelOn, true) : HAL_INVALID_PARAM;
}

hal_status_t HAL_TIM_OC_StopChannel(hal_tim_handle_t *htim, hal_tim_channel_t channel)
{
    return (channel == HAL_TIM_CHANNEL_1) ? fakeSwitch(htim, &htim->channelOn, false) : HAL_INVALID_PARAM;
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

static bool pinLit(void)
{
    return tim1.channelOn && tim1.counterOn && tim1.pulse > 0U;
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
}


/* --- Blink clock --------------------------------------------------------- */

static void testBlinkParams(void)
{
    ledBlink_t b = { 0 };

    check("blink: valid start",               ledBlinkStart(&b, 0U, 100U, 500U, 3U) == LANE_OK);
    b.active = false;
    check("blink: period below minimum",      ledBlinkStart(&b, 0U, LED_BLINK_PERIOD_MIN_MS - 1U, 500U, 3U) == LANE_ERR_INVALID_ARG);
    check("blink: period above maximum",      ledBlinkStart(&b, 0U, LED_BLINK_PERIOD_MAX_MS + 1U, 500U, 3U) == LANE_ERR_INVALID_ARG);
    check("blink: on share 0",                ledBlinkStart(&b, 0U, 100U, 0U, 3U) == LANE_ERR_INVALID_ARG);
    check("blink: on share 1000",             ledBlinkStart(&b, 0U, 100U, 1000U, 3U) == LANE_ERR_INVALID_ARG);
    check("blink: on phase rounds to 0 ms",   ledBlinkStart(&b, 0U, 20U, 10U, 3U) == LANE_ERR_INVALID_ARG);
    check("blink: rejected start leaves it idle", !b.active);
    check("blink: NULL",                      ledBlinkStart(NULL, 0U, 100U, 500U, 3U) == LANE_ERR_INVALID_ARG);
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
}


/* --- Driver --------------------------------------------------------------- */

static void testDriver(void)
{
    tim1 = (struct hostTimHandle){ .period = 999U };
    hostTickMs = 0U;

    check("led init: ok",                     indicatorLedInit() == LANE_OK);
    check("led init: dark, default level programmed",
          !pinLit() && tim1.pulse == ledPatternDutyCounts(INDICATOR_LED_DEFAULT_LEVEL, 1000U));

    check("led on",                           indicatorLedSet(true) == LANE_OK && pinLit());
    check("led on again: no HAL misuse",      indicatorLedSet(true) == LANE_OK && tim1.misuse == 0);
    check("led level while lit",              indicatorLedSetLevel(500U) == LANE_OK
                                              && tim1.pulse == ledPatternDutyCounts(500U, 1000U));
    check("led off parks dark",               indicatorLedSet(false) == LANE_OK && !tim1.channelOn && !tim1.counterOn);
    check("led off again: no HAL misuse",     indicatorLedSet(false) == LANE_OK && tim1.misuse == 0);
    check("led level while dark is kept",     indicatorLedSetLevel(100U) == LANE_OK && !pinLit());
    check("led on uses the kept level",       indicatorLedSet(true) == LANE_OK
                                              && tim1.pulse == ledPatternDutyCounts(100U, 1000U));
    check("led level clamps",                 indicatorLedSetLevel(5000U) == LANE_OK && tim1.pulse == 1000U);

    tim1.failNext = 1;
    check("led off: HAL failure reported",    indicatorLedSet(false) == LANE_ERR_TIMER);
    check("led off: state not believed",      indicatorLedIsLit());
    check("led off: retry succeeds",          indicatorLedSet(false) == LANE_OK && !tim1.channelOn && !tim1.counterOn);

    tim1.failNext = 1;
    check("led on: channel refused, reported", indicatorLedSet(true) == LANE_ERR_TIMER && !indicatorLedIsLit());
    check("led on: retry reaches lit",        indicatorLedSet(true) == LANE_OK && pinLit());
    (void)indicatorLedSet(false);

    /* Halfway: channel started, counter refused. The retry must finish the
     * switch, not re-start the running channel (the HAL traps on that). */
    tim1.failCounter = 1;
    check("led on: counter refused, reported", indicatorLedSet(true) == LANE_ERR_TIMER
                                               && tim1.channelOn && !indicatorLedIsLit());
    check("led on: halfway retry completes",   indicatorLedSet(true) == LANE_OK && pinLit()
                                               && tim1.misuse == 0);

    tim1.failCounter = 1;
    check("led off: counter refused, pin already dark",
          indicatorLedSet(false) == LANE_ERR_TIMER && !tim1.channelOn && !pinLit());
    check("led off: halfway retry completes",  indicatorLedSet(false) == LANE_OK && !tim1.counterOn
                                               && tim1.misuse == 0);
    (void)indicatorLedSet(true);

    check("led toggle",                       indicatorLedToggle() == LANE_OK && !indicatorLedIsLit());
    check("no HAL misuse so far",             tim1.misuse == 0);
}

static void testDriverBlink(void)
{
    tim1 = (struct hostTimHandle){ .period = 999U };
    hostTickMs = 500U;
    (void)indicatorLedInit();

    check("led blink: starts lit",            indicatorLedBlink(100U, 500U, 2U) == LANE_OK && pinLit());
    hostTickMs = 560U;
    check("led blink: dark phase",            indicatorLedRun(hostTickMs) == LANE_OK && !pinLit());
    hostTickMs = 600U;
    check("led blink: second flash",          indicatorLedRun(hostTickMs) == LANE_OK && pinLit());
    hostTickMs = 700U;
    check("led blink: ends dark",             indicatorLedRun(hostTickMs) == LANE_OK && !pinLit()
                                              && !indicatorLedIsBlinking());

    check("led blink: bad params refused",    indicatorLedBlink(5U, 500U, 2U) == LANE_ERR_INVALID_ARG
                                              && !indicatorLedIsBlinking());

    (void)indicatorLedBlink(100U, 500U, 0U);
    check("led set cancels blink",            indicatorLedSet(false) == LANE_OK && !indicatorLedIsBlinking());
    hostTickMs = 800U;
    check("led stays dark after cancel",      indicatorLedRun(hostTickMs) == LANE_OK && !pinLit());
    check("blink: no HAL misuse",             tim1.misuse == 0);
}

int main(void)
{
    testGamma();
    testBlinkParams();
    testBlinkClock();
    testDriver();
    testDriverBlink();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
