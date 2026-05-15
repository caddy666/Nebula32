#pragma once
// Host-build stub for pico/assert.h
// hard_assert() is always-on in the Pico SDK (unlike assert, never compiled away).
// On the host test runner it aborts so CppUTest's test isolation still works.
#include <stdio.h>
#include <stdlib.h>

#define hard_assert(condition, ...) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "hard_assert failed: " #condition \
                    " (%s:%d)\n", __FILE__, __LINE__); \
            abort(); \
        } \
    } while (0)
