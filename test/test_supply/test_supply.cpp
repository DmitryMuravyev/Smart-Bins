#include <unity.h>

#include "SupplyGate.h"

using sb::SupplyGate;
using Fault = SupplyGate::Fault;

void setUp() {}
void tearDown() {}

static SupplyGate gate() {
  SupplyGate g;
  g.configure(1000, 300, 2000);
  return g;
}

// Without PG: on when wanted, up 300 ms later, off 1 s after the last want.
static void test_without_pg() {
  SupplyGate g = gate();
  SupplyGate::Step s = g.update(false, false, false, 0);
  TEST_ASSERT_FALSE(s.psOn);
  s = g.update(true, false, false, 100);
  TEST_ASSERT_TRUE(s.psOn);
  TEST_ASSERT_FALSE(s.ready);
  s = g.update(true, false, false, 399);
  TEST_ASSERT_FALSE(s.ready);
  s = g.update(true, false, false, 400);
  TEST_ASSERT_TRUE(s.ready);
  s = g.update(false, false, false, 1399);
  TEST_ASSERT_TRUE(s.psOn);
  s = g.update(false, false, false, 1400);
  TEST_ASSERT_FALSE(s.psOn);
  TEST_ASSERT_FALSE(s.ready);
}

// A want within the off delay keeps the output on and up (no new settling).
static void test_rewant_within_delay() {
  SupplyGate g = gate();
  g.update(true, false, false, 0);
  g.update(true, false, false, 500);
  SupplyGate::Step s = g.update(false, false, false, 600);
  s = g.update(true, false, false, 1200);
  TEST_ASSERT_TRUE(s.psOn);
  TEST_ASSERT_TRUE(s.ready);
}

// With PG: up exactly when PG is high.
static void test_with_pg() {
  SupplyGate g = gate();
  SupplyGate::Step s = g.update(true, true, false, 0);
  TEST_ASSERT_TRUE(s.psOn);
  TEST_ASSERT_FALSE(s.ready);
  s = g.update(true, true, false, 1500);
  TEST_ASSERT_FALSE(s.ready);
  TEST_ASSERT_TRUE(s.fault == Fault::None);
  s = g.update(true, true, true, 1600);
  TEST_ASSERT_TRUE(s.ready);
}

// PG dropping while on is reported once; after the timeout the lights carry on.
static void test_pg_lost() {
  SupplyGate g = gate();
  g.update(true, true, true, 0);
  SupplyGate::Step s = g.update(true, true, false, 100);
  TEST_ASSERT_TRUE(s.fault == Fault::PgLost);
  TEST_ASSERT_FALSE(s.ready);
  s = g.update(true, true, false, 200);
  TEST_ASSERT_TRUE(s.fault == Fault::None);
  s = g.update(true, true, false, 2100);
  TEST_ASSERT_TRUE(s.fault == Fault::PgLate);
  TEST_ASSERT_TRUE(s.ready);
  s = g.update(true, true, false, 3000);
  TEST_ASSERT_TRUE(s.fault == Fault::None);
  TEST_ASSERT_TRUE(s.ready);
}

// A PG that never comes up: one fault, then the output counts as up.
static void test_pg_never_up() {
  SupplyGate g = gate();
  SupplyGate::Step s = g.update(true, true, false, 0);
  s = g.update(true, true, false, 1999);
  TEST_ASSERT_FALSE(s.ready);
  s = g.update(true, true, false, 2000);
  TEST_ASSERT_TRUE(s.fault == Fault::PgLate);
  TEST_ASSERT_TRUE(s.ready);
  s = g.update(true, true, false, 2500);
  TEST_ASSERT_TRUE(s.fault == Fault::None);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_without_pg);
  RUN_TEST(test_rewant_within_delay);
  RUN_TEST(test_with_pg);
  RUN_TEST(test_pg_lost);
  RUN_TEST(test_pg_never_up);
  return UNITY_END();
}
