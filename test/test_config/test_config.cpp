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
  a.lights[0].pixels = 144;
  a.lights[0].chipset = Chipset::SK6812_RGBW;
  strcpy(a.lights[0].colorOrder, "RGB");
  a.lights[0].invertData = true;
  a.lights[0].currentLimitMa = 3000;
  a.lights[3].maxBrightness = 180;
  a.lights[3].mode = LightMode::White;
  a.locate.color = 0x123ABC;
  a.locate.mode = LocateMode::FastPulse;
  a.effects.speed = 200;
  a.effects.palette = EffectPalette::Lava;
  a.effects.timerMin = 45;
  a.grid.invertData = true;

  std::string json = serialize(a);
  AppConfig b;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parse(json.c_str(), b, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(json.c_str(), serialize(b).c_str());
  TEST_ASSERT_EQUAL_STRING("lab-bins", b.deviceName);
  TEST_ASSERT_EQUAL_STRING("ru", b.language);
  TEST_ASSERT_TRUE(b.lights[0].mode == LightMode::Addressable);
  TEST_ASSERT_EQUAL(144, b.lights[0].pixels);
  TEST_ASSERT_TRUE(b.lights[0].chipset == Chipset::SK6812_RGBW);
  TEST_ASSERT_EQUAL_STRING("RGB", b.lights[0].colorOrder);
  TEST_ASSERT_TRUE(b.lights[0].invertData);
  TEST_ASSERT_EQUAL(3000, b.lights[0].currentLimitMa);
  TEST_ASSERT_EQUAL(180, b.lights[3].maxBrightness);
  TEST_ASSERT_EQUAL_HEX32(0x123ABC, b.locate.color);
  TEST_ASSERT_EQUAL(200, b.effects.speed);
  TEST_ASSERT_EQUAL(EffectPalette::Lava, b.effects.palette);
  TEST_ASSERT_EQUAL(45, b.effects.timerMin);
  TEST_ASSERT_EQUAL(128, b.effects.intensity);
  TEST_ASSERT_TRUE(b.grid.invertData);
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
  expectError("{\"effects\":{\"palette\":\"neon\"}}", "effects.palette");
  expectError("{\"effects\":{\"speed\":0}}", "effects.speed");
  expectError("{\"grid\":{\"invertData\":1}}", "grid.invertData must be true or false");
  expectError("{\"grid\":{\"cols\":64,\"rows\":64,\"pixelsPerBin\":1}}", "maximum is 1500");
}

static void test_addressable_light_rules() {
  expectError("{\"lights\":[{\"mode\":\"addressable\",\"pixels\":0}]}", "lights[0].pixels");
  expectError("{\"lights\":[{\"mode\":\"addressable\",\"colorOrder\":\"RRB\"}]}", "lights[0].colorOrder");
  expectError("{\"lights\":[{\"maxBrightness\":0}]}", "lights[0].maxBrightness");
  expectError("{\"lights\":[{},{\"chipset\":\"APA102\"}]}", "lights[1].chipset");
  // An unused channel's strip settings are not checked: only addressable mode uses them.
  AppConfig cfg;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"lights\":[{\"mode\":\"white\",\"pixels\":0}]}", cfg, err), err.c_str());
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

static void test_static_ip() {
  AppConfig cfg;
  std::string err;
  const char *ok = "{\"network\":{\"mode\":\"static\",\"ip\":\"192.168.1.50\",\"gateway\":\"192.168.1.1\"}}";
  TEST_ASSERT_TRUE_MESSAGE(parse(ok, cfg, err), err.c_str());
  TEST_ASSERT_TRUE(cfg.network.staticIp);
  TEST_ASSERT_EQUAL(50, cfg.network.ip[3]);
  TEST_ASSERT_EQUAL(255, cfg.network.subnet[0]);  // default mask kept
  // Survives a save and reload.
  AppConfig again;
  TEST_ASSERT_TRUE_MESSAGE(parse(serialize(cfg).c_str(), again, err), err.c_str());
  TEST_ASSERT_TRUE(again.network.staticIp);
  TEST_ASSERT_EQUAL(1, again.network.gateway[3]);

  // DHCP ignores whatever addresses are filled in.
  TEST_ASSERT_TRUE(parse("{\"network\":{\"mode\":\"dhcp\"}}", cfg, err));
  TEST_ASSERT_FALSE(cfg.network.staticIp);

  expectError("{\"network\":{\"ip\":\"192.168.1\"}}", "network.ip must be an address");
  expectError("{\"network\":{\"ip\":\"192.168.1.256\"}}", "network.ip must be an address");
  expectError("{\"network\":{\"ip\":\"1.2.3.4x\"}}", "network.ip must be an address");
  expectError("{\"network\":{\"mode\":\"static\",\"ip\":\"192.168.1.50\",\"gateway\":\"192.168.2.1\"}}",
              "network.gateway");
  expectError("{\"network\":{\"mode\":\"static\",\"ip\":\"192.168.1.50\",\"gateway\":\"192.168.1.1\","
              "\"subnet\":\"255.0.255.0\"}}",
              "network.subnet");
  expectError("{\"network\":{\"mode\":\"static\",\"ip\":\"192.168.1.255\",\"gateway\":\"192.168.1.1\"}}",
              "network.ip is not a usable");
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
  RUN_TEST(test_addressable_light_rules);
  RUN_TEST(test_host_names);
  RUN_TEST(test_static_ip);
  return UNITY_END();
}
