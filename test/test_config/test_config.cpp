#include <unity.h>

#include <cstring>
#include <string>

#include "AppConfig.h"

using namespace sb::config;

void setUp() {}
void tearDown() {}

static void test_empty_object_gives_defaults() {
  AppConfig cfg;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parse("{}", cfg, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING("smartbins", cfg.deviceName);
  TEST_ASSERT_EQUAL(10, cfg.grid.cols);
  TEST_ASSERT_EQUAL(6, cfg.grid.rows);
  TEST_ASSERT_EQUAL(2, cfg.grid.pixelsPerBin);
  TEST_ASSERT_TRUE(cfg.grid.chipset == Chipset::WS2815);
  TEST_ASSERT_EQUAL(120, gridPixels(cfg.grid));
  TEST_ASSERT_EQUAL(400, cfg.fadeMs);
}

static void test_round_trip() {
  AppConfig a;
  strcpy(a.deviceName, "lab-bins");
  strcpy(a.language, "ru");
  a.grid.cols = 8;
  a.grid.chipset = Chipset::WS2812B;
  a.grid.voltage = 5;
  a.grid.segments = 2;
  a.grid.splitRow = 2;
  a.lights[0].mode = LightMode::Addressable;
  a.lights[0].pixels = 60;
  a.lights[3].mode = LightMode::White;
  a.locate.color = 0x123ABC;
  a.locate.mode = LocateMode::FastPulse;

  std::string json = serialize(a);
  AppConfig b;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parse(json.c_str(), b, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(json.c_str(), serialize(b).c_str());
  TEST_ASSERT_EQUAL_STRING("lab-bins", b.deviceName);
  TEST_ASSERT_EQUAL_STRING("ru", b.language);
  TEST_ASSERT_TRUE(b.lights[0].mode == LightMode::Addressable);
  TEST_ASSERT_EQUAL(60, b.lights[0].pixels);
  TEST_ASSERT_EQUAL_HEX32(0x123ABC, b.locate.color);
  TEST_ASSERT_EQUAL(32, segmentPixels(b.grid, 0));  // 2 rows x 8 cols x 2 px
  TEST_ASSERT_EQUAL(64, segmentPixels(b.grid, 1));
}

static void test_unknown_keys_ignored() {
  AppConfig cfg;
  std::string err;
  TEST_ASSERT_TRUE(parse("{\"future\":1,\"grid\":{\"cols\":4,\"whatever\":\"x\"}}", cfg, err));
  TEST_ASSERT_EQUAL(4, cfg.grid.cols);
}

static void expectError(const char *json, const char *fragment) {
  AppConfig cfg;
  std::string err;
  TEST_ASSERT_FALSE_MESSAGE(parse(json, cfg, err), json);
  TEST_ASSERT_TRUE_MESSAGE(err.find(fragment) != std::string::npos, err.c_str());
  TEST_ASSERT_EQUAL(10, cfg.grid.cols);  // left at defaults
}

static void test_rejects_bad_values() {
  expectError("not json", "invalid JSON");
  expectError("[]", "JSON object");
  expectError("{\"grid\":{\"cols\":0}}", "grid.cols");
  expectError("{\"grid\":{\"pixelsPerBin\":4}}", "grid.pixelsPerBin");
  expectError("{\"grid\":{\"chipset\":\"APA102\"}}", "grid.chipset must be one of");
  expectError("{\"grid\":{\"voltage\":9}}", "grid.voltage");
  expectError("{\"grid\":{\"colorOrder\":\"RRB\"}}", "grid.colorOrder");
  expectError("{\"grid\":{\"segments\":2,\"splitRow\":6}}", "grid.splitRow");
  expectError("{\"device\":{\"name\":\"Smart Bins\"}}", "device.name");
  expectError("{\"device\":{\"language\":\"de\"}}", "device.language");
  expectError("{\"locate\":{\"color\":\"green\"}}", "locate.color");
  expectError("{\"fadeMs\":\"fast\"}", "fadeMs must be a whole number");
}

static void test_light_channel_rules() {
  expectError("{\"lights\":[{},{},{},{\"mode\":\"addressable\"}]}", "W4");
  expectError("{\"lights\":[{\"mode\":\"rgb\"},{\"mode\":\"rgb\"}]}", "rgb mode needs W1, W2 and W3");
  AppConfig cfg;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(
      parse("{\"lights\":[{\"mode\":\"rgb\"},{\"mode\":\"rgb\"},{\"mode\":\"rgb\"},{\"mode\":\"white\"}]}", cfg, err),
      err.c_str());
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"lights\":[{\"mode\":\"addressable\"},{\"mode\":\"white\"}]}", cfg, err),
                           err.c_str());
}

static void test_host_names() {
  TEST_ASSERT_TRUE(isValidHostName("smartbins"));
  TEST_ASSERT_TRUE(isValidHostName("lab-2"));
  TEST_ASSERT_FALSE(isValidHostName(""));
  TEST_ASSERT_FALSE(isValidHostName("-x"));
  TEST_ASSERT_FALSE(isValidHostName("UPPER"));
  TEST_ASSERT_FALSE(isValidHostName("a.b"));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_object_gives_defaults);
  RUN_TEST(test_round_trip);
  RUN_TEST(test_unknown_keys_ignored);
  RUN_TEST(test_rejects_bad_values);
  RUN_TEST(test_light_channel_rules);
  RUN_TEST(test_host_names);
  return UNITY_END();
}
