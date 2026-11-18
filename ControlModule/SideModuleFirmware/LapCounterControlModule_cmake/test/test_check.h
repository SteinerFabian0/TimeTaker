/*
 * The one assertion helper every suite uses: prints the case name and ok /
 * FAIL, counts failures. Suites return failures != 0.
 */

#pragma once

#include <stdio.h>

static int failures;

static void check(const char *name, int passed)
{
    printf("%-72s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

static int finish(void)
{
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
