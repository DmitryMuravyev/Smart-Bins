#include <unity.h>

#include "PinMap.h"

using namespace sb::board;

void setUp() {}
void tearDown() {}

static void test_default_map_is_valid() {
  PinCheck c = validatePinMap(kDefaultPins, kDefaultPinCount);
  TEST_ASSERT_EQUAL_STRING("ok", pinErrorText(c.error));
}

static void test_rejects_flash_and_console_pins() {
  PinAssignment flash[] = {{"x", 7, PinUse::Output, false}};
  TEST_ASSERT_TRUE(validatePinMap(flash, 1).error == PinError::FlashPin);
  PinAssignment tx[] = {{"x", 1, PinUse::Output, false}};
  TEST_ASSERT_TRUE(validatePinMap(tx, 1).error == PinError::ConsolePin);
}

static void test_rejects_missing_gpio_and_gpio12() {
  PinAssignment missing[] = {{"x", 24, PinUse::Output, false}};
  TEST_ASSERT_TRUE(validatePinMap(missing, 1).error == PinError::NoSuchGpio);
  PinAssignment high[] = {{"x", 40, PinUse::Input, false}};
  TEST_ASSERT_TRUE(validatePinMap(high, 1).error == PinError::NoSuchGpio);
  PinAssignment strap[] = {{"x", 12, PinUse::Output, false}};
  TEST_ASSERT_TRUE(validatePinMap(strap, 1).error == PinError::StrappingPin);
}

static void test_input_only_pins() {
  PinAssignment out[] = {{"x", 35, PinUse::Output, false}};
  TEST_ASSERT_TRUE(validatePinMap(out, 1).error == PinError::InputOnlyUsedAsOutput);
  PinAssignment in[] = {{"x", 35, PinUse::Input, false}};
  TEST_ASSERT_TRUE(validatePinMap(in, 1).error == PinError::None);
}

static void test_duplicates_report_both_entries() {
  PinAssignment dup[] = {
      {"a", 4, PinUse::Output, false},
      {"b", kNoPin, PinUse::Output, false},
      {"c", kNoPin, PinUse::Output, false},
      {"d", 4, PinUse::Output, false},
  };
  PinCheck c = validatePinMap(dup, 4);
  TEST_ASSERT_TRUE(c.error == PinError::Duplicate);
  TEST_ASSERT_EQUAL(3, c.index);
  TEST_ASSERT_EQUAL(0, c.other);
}

// GPIO16/17 carry the PSRAM's clock and chip select on the N16R2 modules: the
// PSRAM build (SB_PSRAM=1) refuses them, the others accept them.
static void test_psram_pins() {
  PinAssignment p[] = {{"x", 16, PinUse::Output, false}, {"y", 17, PinUse::Output, false}};
  const PinError expected = isPsramPin(16) ? PinError::PsramPin : PinError::None;
  TEST_ASSERT_TRUE(validatePinMap(p, 1).error == expected);
  TEST_ASSERT_TRUE(validatePinMap(p + 1, 1).error == expected);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_default_map_is_valid);
  RUN_TEST(test_rejects_flash_and_console_pins);
  RUN_TEST(test_rejects_missing_gpio_and_gpio12);
  RUN_TEST(test_input_only_pins);
  RUN_TEST(test_duplicates_report_both_entries);
  RUN_TEST(test_psram_pins);
  return UNITY_END();
}
