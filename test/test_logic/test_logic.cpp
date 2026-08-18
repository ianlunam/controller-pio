#include <unity.h>

#include <stdlib.h>
#include <time.h>

#include "BinSchedule.h"
#include "MqttPayload.h"

void setUp(void) {}
void tearDown(void) {}

// --- helpers ---------------------------------------------------------------

static struct tm makeDate(int year, int month, int day, int hour = 12) {
    struct tm t = {};
    t.tm_year  = year - 1900;
    t.tm_mon   = month - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_isdst = -1;
    return t;
}

// --- daysSinceBinEpoch -----------------------------------------------------

static void test_epoch_day_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, daysSinceBinEpoch(makeDate(2024, 9, 26)));
}

static void test_next_day_is_one(void) {
    TEST_ASSERT_EQUAL_INT(1, daysSinceBinEpoch(makeDate(2024, 9, 27)));
}

// NZ daylight saving starts on the last Sunday of September, so this fortnight
// contains a 23 hour day. Without normalising to midnight and rounding, the
// hour of skew drags the count to 13.
static void test_fortnight_across_dst_start_is_exactly_14(void) {
    TEST_ASSERT_EQUAL_INT(14, daysSinceBinEpoch(makeDate(2024, 10, 10)));
}

// Time of day must not affect the count: it used to flip at midday because the
// epoch was midnight while "now" carried the current time.
static void test_time_of_day_does_not_change_the_count(void) {
    for (int hour = 0; hour < 24; hour++) {
        TEST_ASSERT_EQUAL_INT(3, daysSinceBinEpoch(makeDate(2024, 9, 29, hour)));
    }
}

static void test_date_before_epoch_is_negative(void) {
    TEST_ASSERT_EQUAL_INT(-1, daysSinceBinEpoch(makeDate(2024, 9, 25)));
}

// --- nextBinType -----------------------------------------------------------

// The circle holds each bin's colour through the morning it goes out, so
// collection day reads as that bin rather than already pointing at the next.
static void test_landfill_collection_day_reads_landfill(void) {
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, nextBinType(0));
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, nextBinType(14));
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, nextBinType(28));
}

static void test_recycle_collection_day_reads_recycle(void) {
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, nextBinType(7));
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, nextBinType(21));
}

static void test_full_cycle_is_seven_days_each(void) {
    int landfill = 0, recycle = 0;
    for (int day = 0; day < BIN_CYCLE_DAYS; day++) {
        if (nextBinType(day) == BIN_LANDFILL) landfill++;
        else recycle++;
    }
    TEST_ASSERT_EQUAL_INT(7, landfill);
    TEST_ASSERT_EQUAL_INT(7, recycle);
}

static void test_days_1_to_7_are_recycle_and_8_to_13_landfill(void) {
    for (int day = 1; day <= 7; day++)  TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, nextBinType(day));
    for (int day = 8; day <= 13; day++) TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, nextBinType(day));
}

// A clock reading a date before the epoch must not fall out of the cycle.
static void test_negative_days_stay_in_cycle(void) {
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, nextBinType(-1));   // phase 13
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, nextBinType(-14));  // phase 0
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE,  nextBinType(-7));   // phase 7
}

// --- parseInt --------------------------------------------------------------

static void test_parses_plain_integer(void) {
    int out = -999;
    TEST_ASSERT_TRUE(parseInt("21", out));
    TEST_ASSERT_EQUAL_INT(21, out);
}

static void test_truncates_a_fraction(void) {
    int out = -999;
    TEST_ASSERT_TRUE(parseInt("12.5", out));
    TEST_ASSERT_EQUAL_INT(12, out);
}

static void test_parses_negative(void) {
    int out = -999;
    TEST_ASSERT_TRUE(parseInt("-3.2", out));
    TEST_ASSERT_EQUAL_INT(-3, out);
}

// These are what Home Assistant publishes when an entity has no value, and
// what used to reboot the board via an uncaught std::stoi throw.
static void test_rejects_home_assistant_placeholders(void) {
    int out = 42;
    TEST_ASSERT_FALSE(parseInt("unknown", out));
    TEST_ASSERT_FALSE(parseInt("unavailable", out));
    TEST_ASSERT_FALSE(parseInt("", out));
    TEST_ASSERT_FALSE(parseInt(nullptr, out));
    TEST_ASSERT_EQUAL_INT(42, out);   // left untouched on failure
}

// --- runner ----------------------------------------------------------------

int main(int, char **) {
    // Pin the zone so the DST assertions are deterministic wherever this runs.
    setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1);
    tzset();

    UNITY_BEGIN();

    RUN_TEST(test_epoch_day_is_zero);
    RUN_TEST(test_next_day_is_one);
    RUN_TEST(test_fortnight_across_dst_start_is_exactly_14);
    RUN_TEST(test_time_of_day_does_not_change_the_count);
    RUN_TEST(test_date_before_epoch_is_negative);

    RUN_TEST(test_landfill_collection_day_reads_landfill);
    RUN_TEST(test_recycle_collection_day_reads_recycle);
    RUN_TEST(test_full_cycle_is_seven_days_each);
    RUN_TEST(test_days_1_to_7_are_recycle_and_8_to_13_landfill);
    RUN_TEST(test_negative_days_stay_in_cycle);

    RUN_TEST(test_parses_plain_integer);
    RUN_TEST(test_truncates_a_fraction);
    RUN_TEST(test_parses_negative);
    RUN_TEST(test_rejects_home_assistant_placeholders);

    return UNITY_END();
}
