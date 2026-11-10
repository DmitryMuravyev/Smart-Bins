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
  a.lights[0].pixelMa = 17;
  a.grid.pixelMa = 40;
  a.lights[3].maxBrightness = 180;
  a.lights[3].mode = LightMode::White;
  a.locate.color = 0x123ABC;
  a.locate.mode = LocateMode::FastPulse;
  a.effects[1].speed = 200;  // sparks
  a.effects[1].palette = EffectPalette::Lava;
  a.effects[1].timerMin = 45;
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
  TEST_ASSERT_EQUAL(17, b.lights[0].pixelMa);
  TEST_ASSERT_EQUAL(40, b.grid.pixelMa);
  TEST_ASSERT_EQUAL(180, b.lights[3].maxBrightness);
  TEST_ASSERT_EQUAL_HEX32(0x123ABC, b.locate.color);
  TEST_ASSERT_EQUAL(200, b.effects[1].speed);
  TEST_ASSERT_EQUAL(EffectPalette::Lava, b.effects[1].palette);
  TEST_ASSERT_EQUAL(45, b.effects[1].timerMin);
  TEST_ASSERT_EQUAL(128, b.effects[1].intensity);
  // Each effect keeps its own settings.
  TEST_ASSERT_EQUAL(128, b.effects[0].speed);
  TEST_ASSERT_EQUAL(EffectPalette::Auto, b.effects[0].palette);
  TEST_ASSERT_EQUAL_STRING("sparks", effectName(1));
  TEST_ASSERT_EQUAL_STRING("demo", effectName(3));
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
  expectError("{\"grid\":{\"pixelsPerBin\":9}}", "grid.pixelsPerBin");
  expectError("{\"grid\":{\"chipset\":\"APA102\"}}", "grid.chipset must be one of");
  expectError("{\"grid\":{\"voltage\":9}}", "grid.voltage");
  expectError("{\"grid\":{\"colorOrder\":\"RRB\"}}", "grid.colorOrder");
  expectError("{\"grid\":{\"segments\":2,\"splitRow\":6}}", "grid.splitRow");
  expectError("{\"device\":{\"name\":\"Smart Bins\"}}", "device.name");
  expectError("{\"device\":{\"language\":\"de\"}}", "device.language");
  expectError("{\"locate\":{\"color\":\"green\"}}", "locate.color");
  expectError("{\"fadeMs\":\"fast\"}", "fadeMs must be a whole number");
  expectError("{\"effects\":{\"fire\":{\"palette\":\"neon\"}}}", "effects.fire.palette");
  expectError("{\"effects\":{\"relax\":{\"speed\":0}}}", "effects.relax.speed");
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

static void test_pins() {
  AppConfig cfg;
  std::string err;
  // Defaults: the board's pin map, written out without the BOOT button.
  TEST_ASSERT_TRUE_MESSAGE(parse("{}", cfg, err), err.c_str());
  TEST_ASSERT_EQUAL(14, cfg.pins["grid_data_a"]);
  TEST_ASSERT_EQUAL(35, cfg.pins["mic_analog"]);
  TEST_ASSERT_EQUAL(-1, cfg.pins["no_such_function"]);
  const std::string json = serialize(cfg);
  TEST_ASSERT_TRUE(json.find("\"grid_data_a\": 14") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"button\"") == std::string::npos);

  // Moving a pin (to the status LED's, which is freed), and back through
  // serialize/parse.
  TEST_ASSERT_TRUE_MESSAGE(
      parse("{\"microphone\":\"off\",\"pins\":{\"light_w4\":2,\"status_led\":-1,\"mic_i2s_sd\":-1}}", cfg, err),
      err.c_str());
  TEST_ASSERT_EQUAL(2, cfg.pins["light_w4"]);
  TEST_ASSERT_EQUAL(-1, cfg.pins["mic_i2s_sd"]);
  AppConfig again;
  TEST_ASSERT_TRUE_MESSAGE(parse(serialize(cfg).c_str(), again, err), err.c_str());
  TEST_ASSERT_EQUAL(2, again.pins["light_w4"]);

  // The board's rules, with the function named in the message.
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"light_w4\":14}}", cfg, err));
  TEST_ASSERT_TRUE_MESSAGE(err.find("light_w4") != std::string::npos && err.find("grid_data_a") != std::string::npos,
                           err.c_str());
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"light_w4\":7}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"light_w4\":35}}", cfg, err));  // input-only
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"light_w4\":12}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"light_w4\":40}}", cfg, err));
  // The microphone: I2S needs its three pins, analog an ADC1 pin (32-39).
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"mic_i2s_sd\":-1}}", cfg, err));
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"microphone\":\"analog\",\"pins\":{\"mic_i2s_sd\":-1}}", cfg, err),
                           err.c_str());
  TEST_ASSERT_TRUE(cfg.microphone == MicType::Analog);
  TEST_ASSERT_TRUE(serialize(cfg).find("\"microphone\": \"analog\"") != std::string::npos);
  TEST_ASSERT_FALSE(parse("{\"microphone\":\"analog\",\"pins\":{\"mic_analog\":-1}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"microphone\":\"analog\",\"pins\":{\"mic_analog\":15}}", cfg, err));  // ADC2
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"microphone\":\"analog\",\"pins\":{\"mic_analog\":36}}", cfg, err),
                           err.c_str());
  TEST_ASSERT_FALSE(parse("{\"microphone\":\"usb\"}", cfg, err));
  // Unknown names, the fixed button and a missing pin for something in use.
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"grid_data_c\":14}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"button\":14}}", cfg, err));
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"pins\":{\"button\":0}}", cfg, err), err.c_str());
  TEST_ASSERT_FALSE(parse("{\"pins\":{\"grid_data_a\":-1}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"grid\":{\"segments\":2},\"pins\":{\"grid_data_b\":-1}}", cfg, err));
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"pins\":{\"light_w5\":-1}}", cfg, err), err.c_str());  // W5 is off
  TEST_ASSERT_FALSE(parse("{\"lights\":[{},{},{},{},{\"mode\":\"white\"}],\"pins\":{\"light_w5\":-1}}", cfg, err));
}

// Motion sensor: off by default, on the analog microphone's pin, so not
// together with the analog microphone.
static void test_motion() {
  AppConfig cfg;
  std::string err;
  TEST_ASSERT_TRUE(parse("{}", cfg, err));
  TEST_ASSERT_FALSE(cfg.motion.enabled);
  TEST_ASSERT_EQUAL(5, cfg.motion.onDelayS);
  TEST_ASSERT_EQUAL(5, cfg.motion.offDelayMin);
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"motion\":{\"enabled\":true,\"onDelayS\":8,\"offDelayMin\":30}}", cfg, err),
                           err.c_str());
  TEST_ASSERT_TRUE(cfg.motion.enabled);
  TEST_ASSERT_EQUAL(8, cfg.motion.onDelayS);
  TEST_ASSERT_EQUAL(30, cfg.motion.offDelayMin);
  AppConfig back;
  TEST_ASSERT_TRUE_MESSAGE(parse(serialize(cfg).c_str(), back, err), err.c_str());
  TEST_ASSERT_TRUE(back.motion.enabled);
  TEST_ASSERT_EQUAL(30, back.motion.offDelayMin);
  TEST_ASSERT_FALSE(parse("{\"motion\":{\"onDelayS\":0}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"motion\":{\"offDelayMin\":241}}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"motion\":{\"enabled\":true},\"microphone\":\"analog\"}", cfg, err));
  TEST_ASSERT_FALSE(parse("{\"motion\":{\"enabled\":true},\"pins\":{\"mic_analog\":-1}}", cfg, err));
  TEST_ASSERT_TRUE_MESSAGE(parse("{\"motion\":{\"enabled\":true},\"microphone\":\"off\"}", cfg, err), err.c_str());
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
  RUN_TEST(test_pins);
  RUN_TEST(test_motion);
  return UNITY_END();
}
