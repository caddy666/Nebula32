// =============================================================================
// test_carousel.cpp — carousel model + .m3u parser (Phase 3)
// =============================================================================
// Verifies the pure-logic core in src/carousel.c: navigation over the ALL and
// PLAYLIST sources (wrap, clamp, empty), position sync, and the .m3u parser
// (comments, blanks, CR/LF, trimming, truncation).
// =============================================================================

#include <CppUTest/TestHarness.h>
#include <string.h>
#include <stdint.h>

extern "C" {
#include "carousel.h"
}

// ===========================================================================
// CarouselNav — model navigation
// ===========================================================================
TEST_GROUP(CarouselNav)
{
    void setup()    { carousel_init(5, 0); }   // ALL over [0,5), pos 0
    void teardown() {}
};

TEST(CarouselNav, InitCountAndStartPos)
{
    CHECK_EQUAL(5, carousel_count());
    CHECK_EQUAL(0, carousel_pos());
    CHECK_EQUAL((int)CAROUSEL_ALL, (int)carousel_get_source());
    CHECK_EQUAL(0, carousel_current_abs());
}

TEST(CarouselNav, InitClampsStartIndex)
{
    carousel_init(3, 99);                       // start beyond range → last entry
    CHECK_EQUAL(2, carousel_pos());
    CHECK_EQUAL(2, carousel_current_abs());
}

TEST(CarouselNav, NextAdvancesThenWraps)
{
    CHECK_EQUAL(1, carousel_next());
    CHECK_EQUAL(2, carousel_next());
    CHECK_EQUAL(3, carousel_next());
    CHECK_EQUAL(4, carousel_next());
    CHECK_EQUAL(0, carousel_next());            // wrap forward 4 → 0
}

TEST(CarouselNav, PrevWrapsBackwardFromStart)
{
    CHECK_EQUAL(4, carousel_prev());            // wrap backward 0 → 4
    CHECK_EQUAL(3, carousel_prev());
}

TEST(CarouselNav, SingleEntryNextPrevStaysPut)
{
    carousel_init(1, 0);
    CHECK_EQUAL(0, carousel_next());
    CHECK_EQUAL(0, carousel_prev());
    CHECK_EQUAL(1, carousel_count());
}

TEST(CarouselNav, EmptyCarouselReturnsMinusOne)
{
    carousel_init(0, 0);
    CHECK_EQUAL(0, carousel_count());
    CHECK_EQUAL(-1, carousel_current_abs());
    CHECK_EQUAL(-1, carousel_next());
    CHECK_EQUAL(-1, carousel_prev());
}

TEST(CarouselNav, SyncPosToAbsValidAndInvalid)
{
    carousel_sync_pos_to_abs(3);
    CHECK_EQUAL(3, carousel_pos());
    CHECK_EQUAL(3, carousel_current_abs());
    carousel_sync_pos_to_abs(99);              // not in carousel → unchanged
    CHECK_EQUAL(3, carousel_pos());
}

TEST(CarouselNav, PlaylistSourceNavigatesAbsIndices)
{
    const uint32_t pl[] = { 7, 2, 40 };
    carousel_set_playlist(pl, 3);
    CHECK_EQUAL((int)CAROUSEL_PLAYLIST, (int)carousel_get_source());
    CHECK_EQUAL(3, carousel_count());
    CHECK_EQUAL(7, carousel_current_abs());     // pos 0 → abs 7
    CHECK_EQUAL(2, carousel_next());            // pos 1 → abs 2
    CHECK_EQUAL(40, carousel_next());           // pos 2 → abs 40
    CHECK_EQUAL(7, carousel_next());            // wrap → abs 7
    CHECK_EQUAL(40, carousel_prev());           // wrap back → abs 40
}

TEST(CarouselNav, PlaylistTruncatedToMaxImages)
{
    uint32_t big[MAX_IMAGES + 8];
    for (int i = 0; i < MAX_IMAGES + 8; i++) big[i] = (uint32_t)i;
    carousel_set_playlist(big, MAX_IMAGES + 8);
    CHECK_EQUAL(MAX_IMAGES, carousel_count());
}

TEST(CarouselNav, UseAllSwitchesBackFromPlaylist)
{
    const uint32_t pl[] = { 1, 2 };
    carousel_set_playlist(pl, 2);
    carousel_use_all(10);
    CHECK_EQUAL((int)CAROUSEL_ALL, (int)carousel_get_source());
    CHECK_EQUAL(10, carousel_count());
}

TEST(CarouselNav, EmptyPlaylistIsSafe)
{
    carousel_set_playlist(NULL, 0);
    CHECK_EQUAL(0, carousel_count());
    CHECK_EQUAL(-1, carousel_next());
}

// ===========================================================================
// CarouselPlaylist — .m3u parser
// ===========================================================================
TEST_GROUP(CarouselPlaylist)
{
    char out[MAX_IMAGES][MAX_PATH_LEN];
    void setup()    { memset(out, 0, sizeof(out)); }
    void teardown() {}
};

TEST(CarouselPlaylist, BasicLinesLF)
{
    const char *m = "disc1.iso\ndisc2.bin\ndisc3.nrg\n";
    int n = carousel_parse_m3u(m, strlen(m), out, MAX_IMAGES);
    CHECK_EQUAL(3, n);
    STRCMP_EQUAL("disc1.iso", out[0]);
    STRCMP_EQUAL("disc2.bin", out[1]);
    STRCMP_EQUAL("disc3.nrg", out[2]);
}

TEST(CarouselPlaylist, CommentsAndBlanksSkipped)
{
    const char *m = "# playlist\n\ndisc1.iso\n; another comment\n   \ndisc2.iso\n";
    int n = carousel_parse_m3u(m, strlen(m), out, MAX_IMAGES);
    CHECK_EQUAL(2, n);
    STRCMP_EQUAL("disc1.iso", out[0]);
    STRCMP_EQUAL("disc2.iso", out[1]);
}

TEST(CarouselPlaylist, CrlfLineEndings)
{
    const char *m = "a.iso\r\nb.iso\r\nc.iso\r\n";
    int n = carousel_parse_m3u(m, strlen(m), out, MAX_IMAGES);
    CHECK_EQUAL(3, n);
    STRCMP_EQUAL("a.iso", out[0]);
    STRCMP_EQUAL("c.iso", out[2]);            // no stray \r left on the line
}

TEST(CarouselPlaylist, LeadingTrailingWhitespaceTrimmed)
{
    const char *m = "  \t spaced.iso \t \n";
    int n = carousel_parse_m3u(m, strlen(m), out, MAX_IMAGES);
    CHECK_EQUAL(1, n);
    STRCMP_EQUAL("spaced.iso", out[0]);
}

TEST(CarouselPlaylist, NoTrailingNewline)
{
    const char *m = "only.iso";
    int n = carousel_parse_m3u(m, strlen(m), out, MAX_IMAGES);
    CHECK_EQUAL(1, n);
    STRCMP_EQUAL("only.iso", out[0]);
}

TEST(CarouselPlaylist, EmptyInputZeroEntries)
{
    int n = carousel_parse_m3u("", 0, out, MAX_IMAGES);
    CHECK_EQUAL(0, n);
}

TEST(CarouselPlaylist, RespectsCap)
{
    const char *m = "1\n2\n3\n4\n5\n";
    int n = carousel_parse_m3u(m, strlen(m), out, 2);   // cap = 2
    CHECK_EQUAL(2, n);
    STRCMP_EQUAL("1", out[0]);
    STRCMP_EQUAL("2", out[1]);
}

TEST(CarouselPlaylist, OverlongLineTruncatedAndNulTerminated)
{
    char big[MAX_PATH_LEN + 64];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\n';
    int n = carousel_parse_m3u(big, sizeof(big), out, MAX_IMAGES);
    CHECK_EQUAL(1, n);
    CHECK_EQUAL((size_t)(MAX_PATH_LEN - 1), strlen(out[0]));   // truncated, NUL-terminated
}

// ===========================================================================
// CarouselPathMatch — basename, case-insensitive matcher (3b)
// ===========================================================================
TEST_GROUP(CarouselPathMatch) { void setup() {} void teardown() {} };

TEST(CarouselPathMatch, ExactBasename)
{
    CHECK_TRUE(carousel_path_matches("Game.iso", "Game.iso"));
}

TEST(CarouselPathMatch, CaseInsensitive)
{
    CHECK_TRUE(carousel_path_matches("game.ISO", "GAME.iso"));
}

TEST(CarouselPathMatch, IgnoresDirectoryComponents)
{
    CHECK_TRUE(carousel_path_matches("Game.iso", "0:/cd/Game.iso"));
    CHECK_TRUE(carousel_path_matches("playlists/../Game.iso", "/mnt/Game.iso"));
    CHECK_TRUE(carousel_path_matches("a\\b\\Game.iso", "c/Game.iso"));   // backslash too
}

TEST(CarouselPathMatch, DifferentNamesDoNotMatch)
{
    CHECK_FALSE(carousel_path_matches("Game1.iso", "Game2.iso"));
    CHECK_FALSE(carousel_path_matches("Game.iso", "Game.bin"));   // ext differs
}

TEST(CarouselPathMatch, PrefixIsNotAMatch)
{
    CHECK_FALSE(carousel_path_matches("Game", "Game.iso"));       // must be full basename
    CHECK_FALSE(carousel_path_matches("Game.iso", "Game.iso2"));
}

TEST(CarouselPathMatch, NullSafe)
{
    CHECK_FALSE(carousel_path_matches(NULL, "x"));
    CHECK_FALSE(carousel_path_matches("x", NULL));
}

// ===========================================================================
// PlaylistMenuWrap — replica of cycle_playlist()'s slot-wrap math (main.c).
// The rotary playlist gesture moves a position through (menu_count + 1) slots
// (slot 0 = "all discs", 1.. = playlists) using a positive modulo so a negative
// detent delta never indexes out of range — the exact trap a bare C '%' hits.
// ===========================================================================
TEST_GROUP(PlaylistMenuWrap) { void setup() {} void teardown() {} };

// Mirrors the one line in cycle_playlist(); kept in lockstep with main.c.
static int slot_move(int pos, int delta, int menu_count)
{
    int slots = menu_count + 1;
    return ((pos + delta) % slots + slots) % slots;
}

TEST(PlaylistMenuWrap, ForwardStep)
{
    // 2 playlists → 3 slots {all, p0, p1}
    CHECK_EQUAL(1, slot_move(0, +1, 2));
    CHECK_EQUAL(2, slot_move(1, +1, 2));
}

TEST(PlaylistMenuWrap, ForwardWrapsToAll)
{
    CHECK_EQUAL(0, slot_move(2, +1, 2));   // last slot → back to "all discs"
}

TEST(PlaylistMenuWrap, BackwardFromAllWrapsToLast)
{
    // The negative-delta case: bare % would yield -1 and index OOB.
    CHECK_EQUAL(2, slot_move(0, -1, 2));
}

TEST(PlaylistMenuWrap, MultiStepDeltaWraps)
{
    CHECK_EQUAL(1, slot_move(0, +4, 2));   // +4 over 3 slots == +1
    CHECK_EQUAL(2, slot_move(0, -4, 2));   // -4 over 3 slots == -1 == slot 2
}

TEST(PlaylistMenuWrap, SinglePlaylistTwoSlots)
{
    CHECK_EQUAL(1, slot_move(0, +1, 1));
    CHECK_EQUAL(0, slot_move(1, +1, 1));
    CHECK_EQUAL(1, slot_move(0, -1, 1));   // wrap back from "all" to the one playlist
}

TEST(PlaylistMenuWrap, LargeNegativeDeltaStaysInRange)
{
    for (int d = -50; d <= 50; d++) {
        int pos = slot_move(0, d, 3);      // 4 slots
        CHECK_TRUE(pos >= 0 && pos < 4);
    }
}
