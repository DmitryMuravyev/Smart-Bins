#include <unity.h>

#include "FormatBytes.h"

static char buf[32];

void setUp() {}
void tearDown() {}

static void test_bytes() {
  TEST_ASSERT_EQUAL_STRING("0 B", sb::formatBytes(0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("1023 B", sb::formatBytes(1023, buf, sizeof(buf)));
}

static void test_kilobytes() {
  TEST_ASSERT_EQUAL_STRING("1.00 KB", sb::formatBytes(1024, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("1.50 KB", sb::formatBytes(1536, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("896.0 KB", sb::formatBytes(0xE0000, buf, sizeof(buf)));
}

static void test_megabytes() {
  TEST_ASSERT_EQUAL_STRING("4.00 MB", sb::formatBytes(4ULL * 1024 * 1024, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("12.1 MB", sb::formatBytes(0xC20000, buf, sizeof(buf)));
}

static void test_small_buffer_is_terminated() {
  char tiny[4];
  sb::formatBytes(123456, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_CHAR('\0', tiny[3]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_bytes);
  RUN_TEST(test_kilobytes);
  RUN_TEST(test_megabytes);
  RUN_TEST(test_small_buffer_is_terminated);
  return UNITY_END();
}
