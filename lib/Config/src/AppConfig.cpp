#include "AppConfig.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace sb {
namespace config {

namespace {

const char *const kChipsets[] = {"WS2812", "WS2812B", "SK6812", "SK6812_RGBW", "WS2811", "WS2815"};
const char *const kLayouts[] = {"serpentine", "progressive"};
const char *const kCorners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
const char *const kLightModes[] = {"off", "white", "rgb", "addressable"};
const char *const kLocateModes[] = {"steady", "slow-pulse", "fast-pulse"};
const char *const kMicTypes[] = {"off", "i2s", "analog"};
const char *const kLanguages[] = {"en", "es", "ru"};
// Same names and order as led::Palette (lib/Led/src/GridEffects.h).
const char *const kPalettes[] = {"auto", "heat", "ocean", "forest", "lava", "rainbow", "party"};
// Same names and order as led::GridEffect (without "off").
const char *const kEffects[kGridEffects] = {"fire", "sparks", "relax", "demo", "spectrum", "beat", "pulse"};

// Field readers: a missing key keeps the current (default) value; a present key
// of the wrong type or out of range fails with the field name in the message.
class Reader {
 public:
  explicit Reader(std::string &error) : error_(error) {}

  bool ok() const { return error_.empty(); }

  template <typename T>
  void integer(JsonVariantConst v, const char *field, long min, long max, T &out) {
    if (!ok() || v.isNull()) return;
    if (!v.is<long>()) return fail(field, "must be a whole number");
    long n = v.as<long>();
    if (n < min || n > max) {
      char msg[64];
      snprintf(msg, sizeof(msg), "must be between %ld and %ld", min, max);
      return fail(field, msg);
    }
    out = static_cast<T>(n);
  }

  template <typename E, size_t N>
  void choice(JsonVariantConst v, const char *field, const char *const (&names)[N], E &out) {
    if (!ok() || v.isNull()) return;
    const char *s = v.as<const char *>();
    if (s != nullptr) {
      for (size_t i = 0; i < N; i++) {
        if (strcmp(s, names[i]) == 0) {
          out = static_cast<E>(i);
          return;
        }
      }
    }
    std::string allowed;
    for (size_t i = 0; i < N; i++) {
      if (i) allowed += ", ";
      allowed += names[i];
    }
    fail(field, ("must be one of: " + allowed).c_str());
  }

  void boolean(JsonVariantConst v, const char *field, bool &out) {
    if (!ok() || v.isNull()) return;
    if (!v.is<bool>()) return fail(field, "must be true or false");
    out = v.as<bool>();
  }

  void text(JsonVariantConst v, const char *field, char *out, size_t size) {
    if (!ok() || v.isNull()) return;
    const char *s = v.as<const char *>();
    if (s == nullptr) return fail(field, "must be a string");
    if (strlen(s) >= size) return fail(field, "is too long");
    strncpy(out, s, size - 1);
    out[size - 1] = '\0';
  }

  void color(JsonVariantConst v, const char *field, uint32_t &out) {
    if (!ok() || v.isNull()) return;
    const char *s = v.as<const char *>();
    unsigned int rgb = 0;
    char tail = 0;
    if (s == nullptr || strlen(s) != 7 || s[0] != '#' || sscanf(s + 1, "%6x%c", &rgb, &tail) != 1) {
      return fail(field, "must be a color like #00FF40");
    }
    out = rgb;
  }

  void ipv4(JsonVariantConst v, const char *field, Ipv4 &out) {
    if (!ok() || v.isNull()) return;
    if (!parseIpv4(v.as<const char *>(), out)) fail(field, "must be an address like 192.168.1.50");
  }

  void fail(const char *field, const char *what) {
    if (!ok()) return;
    error_ = std::string(field) + " " + what;
  }

 private:
  std::string &error_;
};

bool isUniversal(int channel) { return channel < kUniversalChannels; }

uint32_t toU32(const Ipv4 &ip) {
  return (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) | (uint32_t(ip[2]) << 8) | ip[3];
}

}  // namespace

bool parseIpv4(const char *text, Ipv4 &out) {
  if (!text) return false;
  unsigned a, b, c, d;
  char tail;
  // %c after the last number catches trailing junk ("1.2.3.4x"); exactly four
  // conversions means the text was four numbers and nothing else.
  if (sscanf(text, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out[0] = a;
  out[1] = b;
  out[2] = c;
  out[3] = d;
  return true;
}

void formatIpv4(const Ipv4 &ip, char *out) { snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]); }

const char *toString(Chipset v) { return kChipsets[static_cast<int>(v)]; }
const char *toString(Layout v) { return kLayouts[static_cast<int>(v)]; }
const char *toString(StartCorner v) { return kCorners[static_cast<int>(v)]; }
const char *toString(LightMode v) { return kLightModes[static_cast<int>(v)]; }
const char *toString(LocateMode v) { return kLocateModes[static_cast<int>(v)]; }
const char *toString(MicType v) { return kMicTypes[static_cast<int>(v)]; }
const char *toString(EffectPalette v) { return kPalettes[static_cast<int>(v)]; }
const char *effectName(int index) { return index >= 0 && index < kGridEffects ? kEffects[index] : ""; }

bool isValidHostName(const char *name) {
  size_t len = name ? strlen(name) : 0;
  if (len == 0 || len > 32 || name[0] == '-' || name[len - 1] == '-') return false;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

int gridPixels(const GridConfig &grid) { return grid.cols * grid.rows * grid.pixelsPerBin; }

int segmentPixels(const GridConfig &grid, int segment) {
  if (grid.segments < 2) return segment == 0 ? gridPixels(grid) : 0;
  int rows = segment == 0 ? grid.splitRow : grid.rows - grid.splitRow;
  return rows * grid.cols * grid.pixelsPerBin;
}

bool isValidColorOrder(const char *order) {
  return order && strlen(order) == 3 && strchr(order, 'R') && strchr(order, 'G') && strchr(order, 'B');
}

bool validate(const AppConfig &cfg, std::string &error) {
  if (!isValidHostName(cfg.deviceName)) {
    error = "device.name must be 1-32 characters of a-z, 0-9 and '-'";
    return false;
  }
  const NetworkConfig &n = cfg.network;
  if (n.staticIp) {
    const uint32_t ip = toU32(n.ip), gw = toU32(n.gateway), mask = toU32(n.subnet);
    // A valid mask is a run of ones followed by zeros: inverted, plus one, it
    // is a power of two (0xFFFFFF00 -> 0xFF -> 0x100).
    const uint32_t hosts = ~mask;
    if (mask == 0 || (hosts & (hosts + 1)) != 0) {
      error = "network.subnet must be a subnet mask such as 255.255.255.0";
      return false;
    }
    // The address can't be the network or broadcast address of its subnet.
    if ((ip & hosts) == 0 || (ip & hosts) == hosts) {
      error = "network.ip is not a usable address in its subnet";
      return false;
    }
    if ((ip & mask) != (gw & mask) || gw == ip) {
      error = "network.gateway must be another address in the same subnet as network.ip";
      return false;
    }
  }
  const GridConfig &g = cfg.grid;
  if (gridPixels(g) > kMaxGridPixels) {
    error = "grid has " + std::to_string(gridPixels(g)) + " pixels; the maximum is " + std::to_string(kMaxGridPixels);
    return false;
  }
  if (g.segments == 2 && (g.splitRow < 1 || g.splitRow >= g.rows)) {
    error = "grid.splitRow must be between 1 and rows - 1 when using 2 segments";
    return false;
  }
  if (!isValidColorOrder(g.colorOrder)) {
    error = "grid.colorOrder must be a permutation of RGB, e.g. GRB";
    return false;
  }
  // Option 1 (analog RGB strip) uses W1-W3 together; partial RGB makes no sense.
  int rgb = 0;
  for (int i = 0; i < kLightChannels; i++) {
    const LightMode m = cfg.lights[i].mode;
    if (!isUniversal(i) && (m == LightMode::Rgb || m == LightMode::Addressable)) {
      error = "lights[" + std::to_string(i) + "] (W" + std::to_string(i + 1) + ") supports only off or white";
      return false;
    }
    if (m == LightMode::Rgb) rgb++;
    if (m == LightMode::Addressable) {
      const std::string name = "lights[" + std::to_string(i) + "]";
      if (cfg.lights[i].pixels < 1) {
        error = name + ".pixels must be at least 1 for an addressable strip";
        return false;
      }
      if (!isValidColorOrder(cfg.lights[i].colorOrder)) {
        error = name + ".colorOrder must be a permutation of RGB, e.g. GRB";
        return false;
      }
    }
  }
  if (rgb != 0 && rgb != kUniversalChannels) {
    error = "rgb mode needs W1, W2 and W3 all set to rgb";
    return false;
  }
  return validatePins(cfg, error);
}

bool validatePins(const AppConfig &cfg, std::string &error) {
  // The board's rules (flash and console pins, input-only pins as outputs,
  // GPIO12, one function per GPIO), applied to the configured map.
  board::PinAssignment map[board::kDefaultPinCount];
  for (size_t i = 0; i < board::kDefaultPinCount; i++) {
    map[i] = board::kDefaultPins[i];
    map[i].gpio = cfg.pins.gpio[i];
  }
  const board::PinCheck c = board::validatePinMap(map, board::kDefaultPinCount);
  if (c.error != board::PinError::None) {
    error = std::string("pins.") + map[c.index].name + " (GPIO" + std::to_string(map[c.index].gpio) +
            "): " + board::pinErrorText(c.error);
    if (c.error == board::PinError::Duplicate) error += std::string(" (") + map[c.other].name + ")";
    return false;
  }
  // Functions the configuration says are in use need a pin.
  if (cfg.pins["grid_data_a"] == board::kNoPin) {
    error = "pins.grid_data_a: the grid needs a data pin";
    return false;
  }
  if (cfg.grid.segments == 2 && cfg.pins["grid_data_b"] == board::kNoPin) {
    error = "pins.grid_data_b: grid.segments is 2, so the second data line needs a pin";
    return false;
  }
  static const char *const kLightPins[kLightChannels] = {"light_w1", "light_w2", "light_w3", "light_w4", "light_w5"};
  for (int i = 0; i < kLightChannels; i++) {
    if (cfg.lights[i].mode != LightMode::Off && cfg.pins[kLightPins[i]] == board::kNoPin) {
      error = std::string("pins.") + kLightPins[i] + ": W" + std::to_string(i + 1) + " is in use, so it needs a pin";
      return false;
    }
  }
  if (cfg.microphone == MicType::I2s) {
    for (const char *name : {"mic_i2s_sck", "mic_i2s_ws", "mic_i2s_sd"}) {
      if (cfg.pins[name] == board::kNoPin) {
        error = std::string("pins.") + name + ": the I2S microphone needs SCK, WS and SD";
        return false;
      }
    }
  }
  if (cfg.motion.enabled) {
    if (cfg.pins["mic_analog"] == board::kNoPin) {
      error = "pins.mic_analog: the motion sensor is read on this pin, so it needs one";
      return false;
    }
    if (cfg.microphone == MicType::Analog) {
      error = "motion.enabled: the motion sensor uses the analog microphone's pin; choose the I2S microphone or none";
      return false;
    }
  }
  if (cfg.microphone == MicType::Analog) {
    // ADC1 only (GPIO32-39): ADC2 can't be read while Wi-Fi is on.
    const int g = cfg.pins["mic_analog"];
    if (g == board::kNoPin) {
      error = "pins.mic_analog: the analog microphone needs a pin";
      return false;
    }
    if (g < 32) {
      error = "pins.mic_analog (GPIO" + std::to_string(g) + "): must be GPIO32-39 (ADC1; ADC2 doesn't work with Wi-Fi)";
      return false;
    }
  }
  return true;
}

bool parse(const char *json, AppConfig &out, std::string &error) {
  out = AppConfig();
  error.clear();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    error = std::string("invalid JSON: ") + err.c_str();
    return false;
  }
  if (!doc.is<JsonObjectConst>()) {
    error = "config must be a JSON object";
    return false;
  }

  AppConfig cfg;
  Reader r(error);
  int version = kSchemaVersion;
  r.integer(doc["version"], "version", 1, kSchemaVersion, version);

  JsonVariantConst device = doc["device"];
  r.text(device["name"], "device.name", cfg.deviceName, sizeof(cfg.deviceName));
  uint8_t language = 0;
  r.choice(device["language"], "device.language", kLanguages, language);
  if (r.ok() && !device["language"].isNull()) {
    strncpy(cfg.language, kLanguages[language], sizeof(cfg.language) - 1);
  }

  JsonVariantConst grid = doc["grid"];
  GridConfig &g = cfg.grid;
  r.integer(grid["cols"], "grid.cols", 1, 64, g.cols);
  r.integer(grid["rows"], "grid.rows", 1, 64, g.rows);
  r.integer(grid["pixelsPerBin"], "grid.pixelsPerBin", 1, 8, g.pixelsPerBin);
  r.choice(grid["chipset"], "grid.chipset", kChipsets, g.chipset);
  r.text(grid["colorOrder"], "grid.colorOrder", g.colorOrder, sizeof(g.colorOrder));
  r.integer(grid["voltage"], "grid.voltage", 5, 24, g.voltage);
  if (r.ok() && g.voltage != 5 && g.voltage != 12 && g.voltage != 24) r.fail("grid.voltage", "must be 5, 12 or 24");
  r.choice(grid["layout"], "grid.layout", kLayouts, g.layout);
  r.choice(grid["start"], "grid.start", kCorners, g.start);
  r.integer(grid["segments"], "grid.segments", 1, 2, g.segments);
  r.integer(grid["splitRow"], "grid.splitRow", 0, 64, g.splitRow);
  r.integer(grid["brightness"], "grid.brightness", 0, 255, g.brightness);
  r.integer(grid["currentLimitMa"], "grid.currentLimitMa", 100, 30000, g.currentLimitMa);
  r.boolean(grid["invertData"], "grid.invertData", g.invertData);

  JsonVariantConst lights = doc["lights"];
  if (r.ok() && !lights.isNull()) {
    if (!lights.is<JsonArrayConst>() || lights.size() > static_cast<size_t>(kLightChannels)) {
      r.fail("lights", "must be an array of up to 5 channels");
    }
    for (int i = 0; r.ok() && i < kLightChannels && i < static_cast<int>(lights.size()); i++) {
      char field[48];
      JsonVariantConst ch = lights[i];
      LightConfig &l = cfg.lights[i];
      // One buffer for all field names: each reader call uses it before the next snprintf overwrites it.
      snprintf(field, sizeof(field), "lights[%d].mode", i);
      r.choice(ch["mode"], field, kLightModes, l.mode);
      snprintf(field, sizeof(field), "lights[%d].maxBrightness", i);
      r.integer(ch["maxBrightness"], field, 1, 255, l.maxBrightness);
      snprintf(field, sizeof(field), "lights[%d].pixels", i);
      r.integer(ch["pixels"], field, 0, kMaxLightPixels, l.pixels);
      snprintf(field, sizeof(field), "lights[%d].chipset", i);
      r.choice(ch["chipset"], field, kChipsets, l.chipset);
      snprintf(field, sizeof(field), "lights[%d].colorOrder", i);
      r.text(ch["colorOrder"], field, l.colorOrder, sizeof(l.colorOrder));
      snprintf(field, sizeof(field), "lights[%d].invertData", i);
      r.boolean(ch["invertData"], field, l.invertData);
      snprintf(field, sizeof(field), "lights[%d].currentLimitMa", i);
      r.integer(ch["currentLimitMa"], field, 100, 30000, l.currentLimitMa);
    }
  }

  JsonVariantConst network = doc["network"];
  static const char *const kIpModes[] = {"dhcp", "static"};
  uint8_t ipMode = cfg.network.staticIp ? 1 : 0;
  r.choice(network["mode"], "network.mode", kIpModes, ipMode);
  cfg.network.staticIp = ipMode == 1;
  r.ipv4(network["ip"], "network.ip", cfg.network.ip);
  r.ipv4(network["gateway"], "network.gateway", cfg.network.gateway);
  r.ipv4(network["subnet"], "network.subnet", cfg.network.subnet);
  r.ipv4(network["dns"], "network.dns", cfg.network.dns);

  r.integer(doc["fadeMs"], "fadeMs", 0, 5000, cfg.fadeMs);

  JsonVariantConst locate = doc["locate"];
  r.integer(locate["durationS"], "locate.durationS", 0, 3600, cfg.locate.durationS);
  r.color(locate["color"], "locate.color", cfg.locate.color);
  r.choice(locate["mode"], "locate.mode", kLocateModes, cfg.locate.mode);

  // One object per effect: effects.fire.speed and so on.
  JsonVariantConst effects = doc["effects"];
  for (int i = 0; i < kGridEffects; i++) {
    JsonVariantConst e = effects[kEffects[i]];
    EffectSettings &out = cfg.effects[i];
    const std::string f = std::string("effects.") + kEffects[i] + ".";
    r.integer(e["speed"], (f + "speed").c_str(), 1, 255, out.speed);
    r.integer(e["intensity"], (f + "intensity").c_str(), 1, 255, out.intensity);
    r.choice(e["palette"], (f + "palette").c_str(), kPalettes, out.palette);
    r.integer(e["brightness"], (f + "brightness").c_str(), 1, 255, out.brightness);
    r.integer(e["timerMin"], (f + "timerMin").c_str(), 0, 1440, out.timerMin);
  }

  // Pins: an object of function name -> GPIO (-1 = not wired). Names the
  // firmware doesn't know are an error rather than ignored: a typo would
  // otherwise silently leave the old pin in place.
  JsonVariantConst pins = doc["pins"];
  if (r.ok() && !pins.isNull()) {
    if (!pins.is<JsonObjectConst>()) {
      error = "pins must be an object of function name: GPIO";
      return false;
    }
    for (JsonPairConst kv : pins.as<JsonObjectConst>()) {
      const std::string field = std::string("pins.") + kv.key().c_str();
      const int i = board::pinIndex(kv.key().c_str());
      if (i < 0) {
        error = field + " is not a pin function this firmware knows";
        return false;
      }
      if (board::isFixedPin(kv.key().c_str())) {
        if (kv.value().as<long>() != board::kDefaultPins[i].gpio || !kv.value().is<long>()) {
          error = field + " is the BOOT button and stays on GPIO0";
          return false;
        }
        continue;
      }
      r.integer(kv.value(), field.c_str(), -1, 39, cfg.pins.gpio[i]);
    }
  }

  r.choice(doc["microphone"], "microphone", kMicTypes, cfg.microphone);

  JsonVariantConst motion = doc["motion"];
  r.boolean(motion["enabled"], "motion.enabled", cfg.motion.enabled);
  r.integer(motion["onDelayS"], "motion.onDelayS", 1, 60, cfg.motion.onDelayS);
  r.integer(motion["offDelayMin"], "motion.offDelayMin", 1, 240, cfg.motion.offDelayMin);

  if (!r.ok() || !validate(cfg, error)) return false;
  out = cfg;
  return true;
}

std::string serialize(const AppConfig &cfg) {
  JsonDocument doc;
  doc["version"] = kSchemaVersion;

  JsonObject device = doc["device"].to<JsonObject>();
  device["name"] = cfg.deviceName;
  device["language"] = cfg.language;

  // Addresses as text; the buffers stay alive until serializeJsonPretty below,
  // so storing them by pointer (ArduinoJson does that for char arrays) is fine.
  const NetworkConfig &n = cfg.network;
  char ip[16], gateway[16], subnet[16], dns[16];
  formatIpv4(n.ip, ip);
  formatIpv4(n.gateway, gateway);
  formatIpv4(n.subnet, subnet);
  formatIpv4(n.dns, dns);
  JsonObject network = doc["network"].to<JsonObject>();
  network["mode"] = n.staticIp ? "static" : "dhcp";
  network["ip"] = ip;
  network["gateway"] = gateway;
  network["subnet"] = subnet;
  network["dns"] = dns;

  const GridConfig &g = cfg.grid;
  JsonObject grid = doc["grid"].to<JsonObject>();
  grid["cols"] = g.cols;
  grid["rows"] = g.rows;
  grid["pixelsPerBin"] = g.pixelsPerBin;
  grid["chipset"] = toString(g.chipset);
  grid["colorOrder"] = g.colorOrder;
  grid["voltage"] = g.voltage;
  grid["layout"] = toString(g.layout);
  grid["start"] = toString(g.start);
  grid["segments"] = g.segments;
  grid["splitRow"] = g.splitRow;
  grid["brightness"] = g.brightness;
  grid["currentLimitMa"] = g.currentLimitMa;
  grid["invertData"] = g.invertData;

  JsonArray lights = doc["lights"].to<JsonArray>();
  for (const LightConfig &l : cfg.lights) {
    JsonObject ch = lights.add<JsonObject>();
    ch["mode"] = toString(l.mode);
    ch["maxBrightness"] = l.maxBrightness;
    ch["pixels"] = l.pixels;
    ch["chipset"] = toString(l.chipset);
    ch["colorOrder"] = l.colorOrder;
    ch["invertData"] = l.invertData;
    ch["currentLimitMa"] = l.currentLimitMa;
  }

  doc["fadeMs"] = cfg.fadeMs;

  JsonObject locate = doc["locate"].to<JsonObject>();
  locate["durationS"] = cfg.locate.durationS;
  char color[8];
  snprintf(color, sizeof(color), "#%06X", static_cast<unsigned>(cfg.locate.color & 0xFFFFFF));
  locate["color"] = color;
  locate["mode"] = toString(cfg.locate.mode);

  JsonObject effects = doc["effects"].to<JsonObject>();
  for (int i = 0; i < kGridEffects; i++) {
    const EffectSettings &s = cfg.effects[i];
    JsonObject e = effects[kEffects[i]].to<JsonObject>();
    e["speed"] = s.speed;
    e["intensity"] = s.intensity;
    e["palette"] = toString(s.palette);
    e["brightness"] = s.brightness;
    e["timerMin"] = s.timerMin;
  }

  doc["microphone"] = toString(cfg.microphone);

  JsonObject motion = doc["motion"].to<JsonObject>();
  motion["enabled"] = cfg.motion.enabled;
  motion["onDelayS"] = cfg.motion.onDelayS;
  motion["offDelayMin"] = cfg.motion.offDelayMin;

  JsonObject pins = doc["pins"].to<JsonObject>();
  for (size_t i = 0; i < board::kDefaultPinCount; i++) {
    if (board::isFixedPin(board::kDefaultPins[i].name)) continue;
    pins[board::kDefaultPins[i].name] = cfg.pins.gpio[i];
  }

  std::string out;
  serializeJsonPretty(doc, out);
  return out;
}

}  // namespace config
}  // namespace sb
