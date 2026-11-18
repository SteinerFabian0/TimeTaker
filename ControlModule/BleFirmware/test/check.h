/*
 * Minimal check helper for the host suites. Each suite's main() returns
 * checkSummary().
 */

#pragma once

#include <stdio.h>

static int checkFailures;
static int checkCount;

static void check(const char *name, int passed)
{
    checkCount++;
    if (!passed) {
        printf("%-64s FAIL\n", name);
        checkFailures++;
    }
}

static int checkSummary(const char *suite)
{
    printf("%s: %d checks, %d failed\n", suite, checkCount, checkFailures);
    return checkFailures == 0 ? 0 : 1;
}
