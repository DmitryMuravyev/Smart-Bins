#pragma once

// Smart Bins configuration model.
//
// The configuration lives in /config.json on the FAT partition. Parsing is forgiving:
// missing keys keep their defaults and unknown keys are ignored, so a config
// written by an older or newer firmware still loads. Values out of range are
// rejected with a message naming the field.
//
// Wi-Fi credentials are not part of this file; they are kept in NVS so a
// config backup never contains the Wi-Fi password.

#include <cstddef>
#include <cstdint>
#include <string>

#include "PinMap.h"

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
// Which microphone feeds the music effects: the I2S INMP441 on
// pins.mic_i2s_*, an analog module (MAX9814) on pins.mic_analog, or none.
enum class MicType : uint8_t { Off, I2s, Analog };

struct GridConfig {
  uint8_t cols = 10;
  uint8_t rows = 6;
  uint8_t pixelsPerBin = 2;
  Chipset chipset = Chipset::WS2815;
  char colorOrder[4] = "GRB";
  Layout layout = Layout::Serpentine;
  StartCorner start = StartCorner::TopLeft;
  uint8_t segments = 1;  // 1 = data A only, 2 = data A + B
  uint8_t splitRow = 3;  // with 2 segments: first row driven by data B
  uint8_t brightness = 128;
  uint16_t currentLimitMa = 1000;
  // Current of one pixel at full white, in mA, for the current estimate and
  // limit. 0 = the LED type's typical figure (led::defaultPixelMa()); a
  // figure measured on the real strip makes the estimate match it.
  uint8_t pixelMa = 0;
  bool invertData = false;  // for single-transistor (inverting) level shifters
};

constexpr int kMaxGridPixels = 1500;

constexpr int kMaxLightPixels = 1000;  // per addressable work strip

// One work-light channel. Which fields matter depends on `mode`:
//   off          the pin is held low
//   white        `maxBrightness` only
//   rgb          W1, W2 and W3 together drive one analog RGB strip (R, G, B);
//                the group uses W1's `maxBrightness`
//   addressable  every field but `maxBrightness`; the strip's data line is
//                this channel's pin
struct LightConfig {
  LightMode mode = LightMode::Off;
  // Output at 100 % in the UI, 0..255, for white and RGB channels: a PWM
  // channel has no current estimate, so this is the only way to keep a strip
  // that is too bright for the room, or too much for its supply, below full
  // output. Not used by addressable strips: their current limit
  // (currentLimitMa) caps them.
  uint8_t maxBrightness = 255;
  uint16_t pixels = 60;
  Chipset chipset = Chipset::WS2812B;
  char colorOrder[4] = "GRB";
  bool invertData = false;          // single-transistor (inverting) level shifter
  uint16_t currentLimitMa = 2000;   // addressable strips are dimmed to stay under this
  uint8_t pixelMa = 0;              // as GridConfig::pixelMa
};

struct LocateConfig {
  uint16_t durationS = 30;  // 0 = until dismissed
  uint32_t color = 0x00FF40;
  LocateMode mode = LocateMode::Steady;
};

// Grid effects: each effect keeps its own settings, the ones it
// last ran with, and gets them back whenever it is started from the UI or
// the API. Which effect runs is not saved: after a restart the grid comes
// back dark, as with located bins.
enum class EffectPalette : uint8_t { Auto, Heat, Ocean, Forest, Lava, Rainbow, Party };  // = led::Palette

// The effects with settings, in led::GridEffect order without Off.
constexpr int kGridEffects = 7;
// "fire", "sparks", "relax", "demo", "spectrum", "beat", "pulse": the keys
// under effects in config.json.
const char *effectName(int index);

struct EffectSettings {
  uint8_t speed = 128;       // 1..255, 128 = each effect's natural pace
  uint8_t intensity = 128;   // 1..255
  EffectPalette palette = EffectPalette::Auto;
  uint8_t brightness = 255;  // 1..255, on top of grid.brightness
  uint16_t timerMin = 0;     // stop after this many minutes, 0 = keep running
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

// Motion sensor: switches the work lights on when someone stays at
// the bench and off once nobody has been there for a while. Any sensor with
// a digital "someone is here" output works: a 24 GHz radar such as the
// LD2410C (it also notices a person sitting still) or a PIR module such as
// the HC-SR501 or AM312 (in repeat-trigger mode). Its output goes to the
// analog microphone's pin (pins.mic_analog, GPIO35 by default), so the
// analog microphone and the sensor can't be used together.
struct MotionConfig {
  bool enabled = false;
  // The output must stay on this long before the lights come on: someone
  // walking past or stopping for a moment doesn't switch them on.
  uint8_t onDelayS = 5;
  // The lights go off once the output has been off this long.
  uint16_t offDelayMin = 5;
};

// Which GPIO each function uses, in the order of board::kDefaultPins;
// -1 = not wired. Starts as the default pin map. Changes apply after a
// restart. The BOOT button (GPIO0) is fixed and not part of config.json.
struct PinsConfig {
  int8_t gpio[board::kDefaultPinCount];
  PinsConfig() {
    for (size_t i = 0; i < board::kDefaultPinCount; i++) gpio[i] = board::kDefaultPins[i].gpio;
  }
  // GPIO of a named function (see kDefaultPins), -1 when not wired or unknown.
  int8_t operator[](const char *name) const {
    const int i = board::pinIndex(name);
    return i < 0 ? board::kNoPin : gpio[i];
  }
};

struct AppConfig {
  char deviceName[33] = "smartbins";
  char language[3] = "en";
  NetworkConfig network;
  GridConfig grid;
  LightConfig lights[kLightChannels];
  uint16_t fadeMs = 400;
  LocateConfig locate;
  EffectSettings effects[kGridEffects];  // indexed as effectName()
  PinsConfig pins;
  MicType microphone = MicType::I2s;
  MotionConfig motion;
};

// Parses JSON into `out`, starting from defaults. On failure returns false,
// leaves `out` at defaults and writes a short reason into `error`.
bool parse(const char *json, AppConfig &out, std::string &error);

// Checks cross-field rules (also run by parse). Returns false with a reason.
bool validate(const AppConfig &cfg, std::string &error);
// The pin map part of validate(): the board's pin rules, and a pin for every
// function the configuration uses.
bool validatePins(const AppConfig &cfg, std::string &error);

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
const char *toString(MicType v);
const char *toString(EffectPalette v);

}  // namespace config
}  // namespace sb
