#!/usr/bin/env python3
"""Smart Bins power budget calculator.

Estimates current draw for the addressable bin grid and an addressable work strip
and suggests the LED supply size, fuse and firmware current limit.

Example (Dmitry's grid: 10 x 6 bins, 2 pixels per bin, 12 V WS2815):
    python3 tools/power_budget.py --cols 10 --rows 6 --pixels-per-bin 2 --chip ws2815
"""

import argparse
import math

# Full-white current per pixel (one addressable unit) and supply voltage.
# Values are typical datasheet / strip ratings; real strips vary by +-20 %.
CHIPS = {
    "ws2812b": (5.0, 60.0),     # 1 LED per pixel
    "sk6812": (5.0, 60.0),      # RGB; RGBW with white on draws up to ~80 mA
    "ws2811-12v": (12.0, 20.0), # 1 IC drives a segment of 3 LEDs
    "ws2815": (12.0, 25.0),     # 1 LED per pixel, backup data line
    "ws2811-24v": (24.0, 10.0), # 1 IC drives a segment of 6 LEDs
}
MA_IDLE_PER_PIXEL = 1.0  # quiescent current of a dark pixel (cut to 0 by the power switch)
W_ESP32 = 2.5            # ESP32 Wi-Fi peaks + level shifter + mic, at 5 V
BUCK_EFFICIENCY = 0.85
SUPPLY_HEADROOM = 1.25   # keep the supply at <= 80 % of its rating


def wire_gauge(amps: float) -> str:
    if amps <= 3:
        return "22 AWG (0.35 mm2)"
    if amps <= 7:
        return "18 AWG (0.75 mm2)"
    if amps <= 10:
        return "16 AWG (1.5 mm2)"
    return "14 AWG (2.5 mm2), or split into several injection feeds"


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cols", type=int, required=True, help="bins per row (X)")
    p.add_argument("--rows", type=int, required=True, help="rows of bins (Y)")
    # 1-8, the same range the firmware accepts (grid.pixelsPerBin in config.json).
    p.add_argument("--pixels-per-bin", type=int, default=2, choices=range(1, 9), metavar="1-8",
                   help="pixels (addressable units) per bin, 1-8")
    p.add_argument("--chip", choices=sorted(CHIPS), default="ws2812b", help="LED chipset of the grid")
    p.add_argument("--work-pixels", type=int, default=0,
                   help="pixels on addressable work-light strips (same chipset and supply)")
    p.add_argument("--limit-a", type=float, default=None,
                   help="firmware current limit for the addressable LEDs, in amps at the strip voltage")
    a = p.parse_args()

    volts, ma_full = CHIPS[a.chip]
    grid_px = a.cols * a.rows * a.pixels_per_bin
    total_px = grid_px + a.work_pixels
    full_a = total_px * ma_full / 1000
    idle_ma = total_px * MA_IDLE_PER_PIXEL

    # Suggested limit: work strips at full white plus a third of the grid,
    # which covers every effect at a pleasant brightness.
    suggested = (a.work_pixels + grid_px / 3) * ma_full / 1000
    limit_a = a.limit_a if a.limit_a is not None else max(0.5, math.ceil(suggested * 2) / 2)
    limit_a = min(limit_a, full_a)

    esp_a = W_ESP32 / volts / (BUCK_EFFICIENCY if volts > 5 else 1.0)
    supply_a = math.ceil((limit_a + esp_a) * SUPPLY_HEADROOM)
    fuse_a = math.ceil(limit_a * 1.25)

    print(f"Chipset:           {a.chip} ({volts:g} V, ~{ma_full:g} mA per pixel at full white)")
    print(f"Grid:              {a.cols} x {a.rows} bins x {a.pixels_per_bin} pixels = {grid_px} pixels")
    print(f"Addressable work:  {a.work_pixels} pixels")
    print(f"All white, 100 %:  {full_a:.1f} A  ({full_a * volts:.0f} W, theoretical maximum)")
    print(f"All dark:          {idle_ma:.0f} mA  (cut to 0 by the power switches)")
    print(f"Firmware limit:    {limit_a:.1f} A  ({limit_a * volts:.0f} W)")
    print(f"LED supply:        {volts:g} V, >= {supply_a} A  ({supply_a * volts:.0f} W), includes the ESP32")
    if volts > 5:
        print("ESP32 supply:      buck converter {:g} V -> 5 V, >= 1 A".format(volts))
    print(f"Grid fuse:         {fuse_a} A slow-blow")
    print(f"Main LED wiring:   {wire_gauge(limit_a)}, inject power every ~1 m or every row")
    print("Non-addressable work lights: size their supply from the strip's W/m rating.")


if __name__ == "__main__":
    main()
