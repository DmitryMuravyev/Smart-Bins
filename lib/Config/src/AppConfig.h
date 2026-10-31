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

constexpr int kMaxLightPixels = 1000;  // per addressable work strip

// One work-light channel. Which fields matter depends on `mode`:
//   off          the pin is held low
//   white        `maxBrightness` only
//   rgb          W1, W2 and W3 together drive one analog RGB strip (R, G, B);
//                the group uses W1's `maxBrightness`
//   addressable  every field; the strip's data line is this channel's pin
struct LightConfig {
  LightMode mode = LightMode::Off;
  // Output at 100 % in the UI, 0..255. Lets a strip that is too bright for the
  // room be capped once instead of never using the top of the slider.
  uint8_t maxBrightness = 255;
  uint16_t pixels = 60;
  Chipset chipset = Chipset::WS2812B;
  char colorOrder[4] = "GRB";
  bool invertData = false;          // single-transistor (inverting) level shifter
  uint16_t currentLimitMa = 2000;   // addressable strips are dimmed to stay under this
};

struct LocateConfig {
  uint16_t durationS = 30;  // 0 = until dismissed
  uint32_t color = 0x00FF40;
  LocateMode mode = LocateMode::Steady;
};

// IPv4 address as four bytes, most significant first (192.168.1.50 = {192, 168, 1, 50}).
using Ipv4 = uint8_t[4];

// How the board gets its address on the home network. DHCP (the router hands
// one out) unless a fixed address is set, e.g. for a router without
// reservations or a network where .local names don't resolve.
struct NetworkConfig {
  bool staticIp = false;
  Ipv4 ip = {0, 0, 0, 0};
  Ipv4 gateway = {0, 0, 0, 0};
  Ipv4 subnet = {255, 255, 255, 0};
  Ipv4 dns = {0, 0, 0, 0};  // 0.0.0.0 = use the gateway
};

struct AppConfig {
  char deviceName[33] = "smartbins";
  char language[3] = "en";
  NetworkConfig network;
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

// Parses dotted IPv4 text ("192.168.1.50"); false for anything else.
bool parseIpv4(const char *text, Ipv4 &out);
// Writes "a.b.c.d" into `out` (at least 16 bytes).
void formatIpv4(const Ipv4 &ip, char *out);

// True when `order` is three letters using each of R, G and B once (e.g. "GRB").
bool isValidColorOrder(const char *order);

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
