#include <unity.h>

#include "PresenceDetector.h"

using sb::PresenceDetector;
using Event = PresenceDetector::Event;

void setUp() {}
void tearDown() {}

// Feeds `high` every 10 ms from `from` to `to` (exclusive) and returns the
// first event, or None.
static Event feed(PresenceDetector &d, bool high, uint32_t from, uint32_t to, uint32_t *at = nullptr) {
  for (uint32_t t = from; t < to; t += 10) {
    const Event e = d.update(high, t);
    if (e != Event::None) {
      if (at) *at = t;
      return e;
    }
  }
  return Event::None;
}

// Someone walking past: a PIR's 2.5 s pulse is shorter than the 5 s on delay.
static void test_passing_by_is_ignored() {
  PresenceDetector d;
  d.configure(5000, 60000);
  TEST_ASSERT_TRUE(feed(d, false, 0, 1000) == Event::None);
  TEST_ASSERT_TRUE(feed(d, true, 1000, 3500) == Event::None);
  TEST_ASSERT_TRUE(feed(d, false, 3500, 10000) == Event::None);
  // Two passes with a gap: the clock starts over at each one.
  TEST_ASSERT_TRUE(feed(d, true, 10000, 14000) == Event::None);
  TEST_ASSERT_TRUE(feed(d, false, 14000, 14200) == Event::None);
  TEST_ASSERT_TRUE(feed(d, true, 14200, 18000) == Event::None);
  TEST_ASSERT_FALSE(d.present());
}

// Someone sits down: the output stays high past the on delay.
static void test_arrival_after_on_delay() {
  PresenceDetector d;
  d.configure(5000, 60000);
  feed(d, false, 0, 1000);
  uint32_t at = 0;
  TEST_ASSERT_TRUE(feed(d, true, 1000, 20000, &at) == Event::Arrived);
  TEST_ASSERT_EQUAL_UINT32(6000, at);
  TEST_ASSERT_TRUE(d.present());
  // Arrival is reported once.
  TEST_ASSERT_TRUE(feed(d, true, 6010, 20000) == Event::None);
}

// Gaps shorter than the off delay (a PIR between movements) keep the
// presence; a long enough gap ends it.
static void test_leaving_after_off_delay() {
  PresenceDetector d;
  d.configure(1000, 60000);
  feed(d, true, 0, 2000);
  TEST_ASSERT_TRUE(d.present());
  TEST_ASSERT_TRUE(feed(d, false, 2000, 50000) == Event::None);
  TEST_ASSERT_TRUE(feed(d, true, 50000, 51000) == Event::None);
  uint32_t at = 0;
  TEST_ASSERT_TRUE(feed(d, false, 51000, 200000, &at) == Event::Left);
  TEST_ASSERT_EQUAL_UINT32(111000, at);
  TEST_ASSERT_FALSE(d.present());
  // And someone can arrive again.
  TEST_ASSERT_TRUE(feed(d, true, 200000, 202000) == Event::Arrived);
}

// The millisecond clock wraps after 49 days; the delays still count right.
static void test_clock_wrap() {
  PresenceDetector d;
  d.configure(5000, 60000);
  const uint32_t start = 0xFFFFFFFFu - 2000;
  d.update(false, start);
  uint32_t t = start + 10;
  Event e = Event::None;
  for (int i = 0; i < 1000 && e == Event::None; i++, t += 10) e = d.update(true, t);
  TEST_ASSERT_TRUE(e == Event::Arrived);
  TEST_ASSERT_EQUAL_UINT32(start + 10 + 5000, t - 10);
}

// wake(): present at once, and gone after the off delay counted from then.
static void test_wake() {
  PresenceDetector d;
  d.configure(5000, 60000);
  d.wake(1000);
  TEST_ASSERT_TRUE(d.present());
  uint32_t at = 0;
  TEST_ASSERT_TRUE(feed(d, false, 1000, 70000, &at) == Event::Left);
  TEST_ASSERT_EQUAL_UINT32(61000, at);
  // A wake while present but with the output low restarts the off delay.
  d.wake(80000);
  TEST_ASSERT_TRUE(feed(d, false, 80000, 120000) == Event::None);
  d.wake(120000);
  TEST_ASSERT_TRUE(feed(d, false, 120000, 179990) == Event::None);
  TEST_ASSERT_TRUE(feed(d, false, 179990, 190000, &at) == Event::Left);
  TEST_ASSERT_EQUAL_UINT32(180000, at);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_passing_by_is_ignored);
  RUN_TEST(test_arrival_after_on_delay);
  RUN_TEST(test_leaving_after_off_delay);
  RUN_TEST(test_clock_wrap);
  RUN_TEST(test_wake);
  return UNITY_END();
}
