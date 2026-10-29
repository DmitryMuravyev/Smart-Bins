#!/usr/bin/env python3
"""Smart Bins power budget calculator.

Estimates current draw for the addressable bin grid and work lights and
suggests a 5 V supply size and firmware current limit.

Example:
    python3 tools/power_budget.py --cols 10 --rows 6 --leds-per-bin 2 --work-leds 60
"""

import argparse
import math

MA_PER_LED_FULL_WHITE = 60.0  # WS2812B / SK6812 RGB, all channels at 255
MA_PER_LED_IDLE = 1.0         # quiescent current of a dark LED (0.6-1 mA)
MA_ESP32 = 500.0              # ESP32 with Wi-Fi peaks, plus level shifter and mic
SUPPLY_HEADROOM = 1.25        # keep the supply at <= 80 % of its rating


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cols", type=int, required=True, help="bins per row (X)")
    p.add_argument("--rows", type=int, required=True, help="rows of bins (Y)")
    p.add_argument("--leds-per-bin", type=int, default=2, choices=(1, 2, 3))
    p.add_argument("--work-leds", type=int, default=0, help="LEDs on the addressable work-light strip, if any")
    p.add_argument("--limit-a", type=float, default=None,
                   help="firmware current limit for the 5 V LEDs in amps (default: suggested)")
    a = p.parse_args()

    grid_leds = a.cols * a.rows * a.leds_per_bin
    total_leds = grid_leds + a.work_leds
    full_a = total_leds * MA_PER_LED_FULL_WHITE / 1000
    idle_a = total_leds * MA_PER_LED_IDLE / 1000

    # Default suggestion: enough for the work strip at full white plus a third of the grid,
    # which covers every effect at a pleasant brightness.
    suggested_limit = a.work_leds * MA_PER_LED_FULL_WHITE / 1000 + grid_leds * MA_PER_LED_FULL_WHITE / 1000 / 3
    limit_a = a.limit_a if a.limit_a is not None else max(1.0, round(suggested_limit * 2) / 2)
    limit_a = min(limit_a, full_a)
    supply_a = math.ceil((limit_a + MA_ESP32 / 1000) * SUPPLY_HEADROOM)

    print(f"Grid:              {a.cols} x {a.rows} bins x {a.leds_per_bin} LED = {grid_leds} LEDs")
    print(f"Addressable work:  {a.work_leds} LEDs")
    print(f"All white, 100 %:  {full_a:.1f} A  (theoretical maximum, never needed)")
    print(f"All dark:          {idle_a * 1000:.0f} mA  (cut to 0 by the power switches)")
    print(f"Firmware limit:    {limit_a:.1f} A")
    print(f"5 V supply:        >= {supply_a} A  ({supply_a * 5} W)")
    gauge = "18 AWG (0.75 mm2)" if limit_a <= 7 else "16 AWG (1.5 mm2)"
    print(f"Main 5 V wiring:   {gauge}, inject power every ~1 m or every row")
    print("Non-addressable work lights run from their own 12/24 V supply; size it from the strip's W/m rating.")


if __name__ == "__main__":
    main()
