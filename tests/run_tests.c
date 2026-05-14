#include <stdio.h>
#include "test_runner.h"

int g_tests_run    = 0;
int g_tests_failed = 0;
const char *g_suite = "";

// PIO stub capture register — written by pio_sm_set_clkdiv() in the stub,
// read by test_da_speed.c to verify the correct clock divider was used.
float g_stub_last_clkdiv = 0.0f;

void test_cd_types(void);
void test_ecc(void);
void test_subcode(void);
void test_da_speed(void);

int main(void) {
    printf("cd32_ode test suite\n");
    printf("===================\n");

    test_cd_types();
    test_ecc();
    test_subcode();
    test_da_speed();

    printf("\n===================\n");
    printf("%d/%d passed", g_tests_run - g_tests_failed, g_tests_run);
    if (g_tests_failed > 0)
        printf("  (%d FAILED)", g_tests_failed);
    printf("\n");

    return g_tests_failed > 0 ? 1 : 0;
}
