#pragma once
#include <stdio.h>
#include <stdint.h>

extern int g_tests_run;
extern int g_tests_failed;
extern const char *g_suite;

#define ASSERT_EQ(actual, expected, msg)                                      \
    do {                                                                       \
        g_tests_run++;                                                         \
        unsigned _a = (unsigned)(actual);                                      \
        unsigned _e = (unsigned)(expected);                                    \
        if (_a != _e) {                                                        \
            printf("  FAIL  %s\n"                                              \
                   "        got 0x%02X (%u), expected 0x%02X (%u)\n",         \
                   msg, _a, _a, _e, _e);                                       \
            g_tests_failed++;                                                  \
        } else {                                                               \
            printf("  pass  %s\n", msg);                                       \
        }                                                                      \
    } while (0)

#define ASSERT_TRUE(cond, msg)                                                 \
    do {                                                                       \
        g_tests_run++;                                                         \
        if (!(cond)) {                                                         \
            printf("  FAIL  %s\n", msg);                                       \
            g_tests_failed++;                                                  \
        } else {                                                               \
            printf("  pass  %s\n", msg);                                       \
        }                                                                      \
    } while (0)

#define ASSERT_FALSE(cond, msg) ASSERT_TRUE(!(cond), msg)

#define SUITE(name)                                                            \
    do {                                                                       \
        g_suite = (name);                                                      \
        printf("\n[%s]\n", g_suite);                                           \
    } while (0)
