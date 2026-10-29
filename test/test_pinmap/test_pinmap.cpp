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
      {"a", 16, PinUse::Output, false},
      {"b", kNoPin, PinUse::Output, false},
      {"c", kNoPin, PinUse::Output, false},
      {"d", 16, PinUse::Output, false},
  };
  PinCheck c = validatePinMap(dup, 4);
  TEST_ASSERT_TRUE(c.error == PinError::Duplicate);
  TEST_ASSERT_EQUAL(3, c.index);
  TEST_ASSERT_EQUAL(0, c.other);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_default_map_is_valid);
  RUN_TEST(test_rejects_flash_and_console_pins);
  RUN_TEST(test_rejects_missing_gpio_and_gpio12);
  RUN_TEST(test_input_only_pins);
  RUN_TEST(test_duplicates_report_both_entries);
  return UNITY_END();
}
