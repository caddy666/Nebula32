#include "test_runner.h"
#include "cd_types.h"

void test_cd_types(void) {
    SUITE("cd_types: lba_to_msf");

    // LBA 0 sits at absolute disc address 00:02:00 — 2-second lead-in offset
    msf_t m = lba_to_msf(0);
    ASSERT_EQ(m.minute, 0x00, "lba_to_msf(0).minute = 0x00");
    ASSERT_EQ(m.second, 0x02, "lba_to_msf(0).second = 0x02  (lead-in)");
    ASSERT_EQ(m.frame,  0x00, "lba_to_msf(0).frame  = 0x00");

    // LBA 75 = 1 second of content → absolute 00:03:00
    m = lba_to_msf(75);
    ASSERT_EQ(m.minute, 0x00, "lba_to_msf(75).minute = 0x00");
    ASSERT_EQ(m.second, 0x03, "lba_to_msf(75).second = 0x03");
    ASSERT_EQ(m.frame,  0x00, "lba_to_msf(75).frame  = 0x00");

    // Frame count ≥ 10 must BCD-encode into two nibbles
    m = lba_to_msf(10);  // total=160, frm=160%75=10, sec=2, min=0
    ASSERT_EQ(m.frame, 0x10, "lba_to_msf(10).frame BCD = 0x10");

    // 1-minute boundary: LBA 4500 = 60 s × 75 fps → absolute 01:02:00
    m = lba_to_msf(4500);
    ASSERT_EQ(m.minute, 0x01, "lba_to_msf(4500).minute = 0x01");
    ASSERT_EQ(m.second, 0x02, "lba_to_msf(4500).second = 0x02");
    ASSERT_EQ(m.frame,  0x00, "lba_to_msf(4500).frame  = 0x00");

    // BCD tens digit in minutes: LBA 54000 = 12 min absolute (10+2)
    m = lba_to_msf(54000);  // total=54150, min=54150/4500=12, sec=0, frm=0
    ASSERT_EQ(m.minute, 0x12, "lba_to_msf(54000).minute BCD = 0x12");

    SUITE("cd_types: msf_to_lba");

    // Minimum disc address: BCD 00:02:00 → LBA 0
    msf_t lead = { 0x00, 0x02, 0x00 };
    ASSERT_EQ(msf_to_lba(lead), 0u, "msf_to_lba(00:02:00) = 0");

    // Address before lead-in must clamp to 0, not underflow
    msf_t before = { 0x00, 0x01, 0x00 };
    ASSERT_EQ(msf_to_lba(before), 0u, "msf_to_lba(00:01:00) clamps to 0");

    msf_t zero_addr = { 0x00, 0x00, 0x00 };
    ASSERT_EQ(msf_to_lba(zero_addr), 0u, "msf_to_lba(00:00:00) clamps to 0");

    SUITE("cd_types: round-trip LBA -> MSF -> LBA");

    uint32_t cases[] = { 0, 1, 74, 75, 149, 150, 1234, 4500, 44999 };
    for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        uint32_t lba = cases[i];
        uint32_t rt  = msf_to_lba(lba_to_msf(lba));
        // Print manually since ASSERT_EQ expects a string literal msg
        g_tests_run++;
        if (rt != lba) {
            printf("  FAIL  round-trip LBA %lu: got %lu\n",
                   (unsigned long)lba, (unsigned long)rt);
            g_tests_failed++;
        } else {
            printf("  pass  round-trip LBA %lu\n", (unsigned long)lba);
        }
    }
}
