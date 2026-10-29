#pragma once

// Smart Bins configuration model.
//
// The configuration lives in /config.json on LittleFS. Parsing is forgiving:
// missing keys keep their defaults and unknown keys are ignored, so a config
// written by an older or newer firmware still loads. Values out of range are
// rejected with a message naming the field.
//
// Wi-Fi credentials are not part of this file; they are kept in NVS so a
// config backup never contains the Wi-Fi password.

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb {
namespace config {

constexpr int kSchemaVersion = 1;
constexpr int kLightChannels = 5;      // W1-W5
constexpr int kUniversalChannels = 3;  // W1-W3 can be addressable or analog RGB

enum class Chipset : uint8_t { WS2812, WS2812B, SK6812, SK6812_RGBW, WS2811, WS2815 };
enum class Layout : uint8_t { Serpentine, Progressive };
enum class StartCorner : uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };
enum class LightMode : uint8_t { Off, White, Rgb, Addressable };
enum class LocateMode : uint8_t { Steady, SlowPulse, FastPulse };

struct GridConfig {
  uint8_t cols = 10;
  uint8_t rows = 6;
  uint8_t pixelsPerBin = 2;
  Chipset chipset = Chipset::WS2815;
  char colorOrder[4] = "GRB";
  uint8_t voltage = 12;  // strip supply: 5, 12 or 24 V (used for the power limit)
  Layout layout = Layout::Serpentine;
  StartCorner start = StartCorner::TopLeft;
  uint8_t segments = 1;  // 1 = data A only, 2 = data A + B
  uint8_t splitRow = 3;  // with 2 segments: first row driven by data B
  uint8_t brightness = 128;
  uint16_t currentLimitMa = 1000;
  bool invertData = false;  // for single-transistor (inverting) level shifters
};

constexpr int kMaxGridPixels = 1500;

struct LightConfig {
  LightMode mode = LightMode::Off;
  uint8_t brightness = 200;
  uint16_t pixels = 0;  // addressable mode only
};

struct LocateConfig {
  uint16_t durationS = 30;  // 0 = until dismissed
  uint32_t color = 0x00FF40;
  LocateMode mode = LocateMode::Steady;
};

struct AppConfig {
  char deviceName[33] = "smartbins";
  char language[3] = "en";
  GridConfig grid;
  LightConfig lights[kLightChannels];
  uint16_t fadeMs = 400;
  LocateConfig locate;
};

// Parses JSON into `out`, starting from defaults. On failure returns false,
// leaves `out` at defaults and writes a short reason into `error`.
bool parse(const char *json, AppConfig &out, std::string &error);

// Checks cross-field rules (also run by parse). Returns false with a reason.
bool validate(const AppConfig &cfg, std::string &error);

// Serializes the full configuration, including defaults, as pretty JSON.
std::string serialize(const AppConfig &cfg);

// Total pixels on the grid and on one data segment.
int gridPixels(const GridConfig &grid);
int segmentPixels(const GridConfig &grid, int segment);

// True for a valid mDNS / DHCP host name: 1-32 chars of a-z, 0-9 and '-', not starting or ending with '-'.
bool isValidHostName(const char *name);

const char *toString(Chipset v);
const char *toString(Layout v);
const char *toString(StartCorner v);
const char *toString(LightMode v);
const char *toString(LocateMode v);

}  // namespace config
}  // namespace sb
