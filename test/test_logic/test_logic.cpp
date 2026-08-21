#include <unity.h>

#include "BinDay.h"
#include "MqttPayload.h"

void setUp(void) {}
void tearDown(void) {}

// --- binTypeFromPayload ----------------------------------------------------

// The exact strings the sensor.bin_day template publishes today.
static void test_the_two_payloads_the_template_actually_sends(void) {
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, binTypeFromPayload("Landfill"));
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE,  binTypeFromPayload("Recycles"));
}

// Reworded templates should keep working rather than silently blanking.
static void test_recycle_wordings(void) {
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, binTypeFromPayload("Recycle"));
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, binTypeFromPayload("Recycling"));
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, binTypeFromPayload("recycles"));
    TEST_ASSERT_EQUAL_INT(BIN_RECYCLE, binTypeFromPayload("RECYCLES"));
}

static void test_landfill_case_insensitive(void) {
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, binTypeFromPayload("landfill"));
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, binTypeFromPayload("LANDFILL"));
}

// Several Home Assistant friendly names on this broker carry a trailing space,
// so tolerate stray whitespace on the leading edge too.
static void test_leading_space_tolerated(void) {
    TEST_ASSERT_EQUAL_INT(BIN_LANDFILL, binTypeFromPayload("  Landfill"));
}

// Home Assistant publishes these when a template has not evaluated yet. The
// caller holds its last good value on BIN_UNKNOWN rather than clearing the
// circle, so these must not be mistaken for a bin.
static void test_placeholders_and_junk_are_unknown(void) {
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload("unknown"));
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload("unavailable"));
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload(""));
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload(nullptr));
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload("Garden"));
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload("Land"));
    TEST_ASSERT_EQUAL_INT(BIN_UNKNOWN, binTypeFromPayload("rec"));
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
    TEST_ASSERT_EQUAL_INT(42, out);  // left untouched on failure
}

// --- runner ----------------------------------------------------------------

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_the_two_payloads_the_template_actually_sends);
    RUN_TEST(test_recycle_wordings);
    RUN_TEST(test_landfill_case_insensitive);
    RUN_TEST(test_leading_space_tolerated);
    RUN_TEST(test_placeholders_and_junk_are_unknown);

    RUN_TEST(test_parses_plain_integer);
    RUN_TEST(test_truncates_a_fraction);
    RUN_TEST(test_parses_negative);
    RUN_TEST(test_rejects_home_assistant_placeholders);

    return UNITY_END();
}
