#!/usr/bin/env python3
"""Fills a Smart Bins board with realistic test parts over its web API.

For trying out search, filters and sorting with more than a handful of parts.
The parts are a mix of common components (resistors, capacitors, MCUs, logic,
regulators, transistors, sensors, connectors) with descriptions in English,
Spanish and Russian, spread over the bins of the grid. Every generated part
has "[test data]" in its notes, so --clear can find and remove them again
without touching parts you entered yourself.

Needs only Python 3 (no extra packages). Examples:
    python3 tools/seed_parts.py                       # 300 parts to smartbins.local
    python3 tools/seed_parts.py --host 192.168.1.10 --count 1000
    python3 tools/seed_parts.py --clear               # remove the test parts
    python3 tools/seed_parts.py --password secret     # when a web password is set

Each part is one database write on the board (a fraction of a second), so a
few hundred parts take a minute or two. The LED strips pause briefly during
each write.
"""

import argparse
import json
import random
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

MARKER = "[test data]"  # in the notes of every generated part; what --clear looks for

# Standard resistor values (E12 series, one decade) and the decades used.
E12 = [1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2]
SMD_PACKAGES = ["0402", "0603", "0805", "1206"]

# Words in the three UI languages, so search can be tried with each alphabet.
RESISTOR = {"en": "Resistor", "es": "Resistencia", "ru": "Резистор"}
CAPACITOR = {"en": "Ceramic capacitor", "es": "Condensador cerámico", "ru": "Конденсатор керамический"}
ELECTROLYTIC = {"en": "Electrolytic capacitor", "es": "Condensador electrolítico", "ru": "Конденсатор электролитический"}
OHM = {"en": "Ω", "es": "Ω", "ru": "Ом"}

# Discrete parts and ICs: part number, manufacturer, category, package, description, datasheet URL.
CATALOGUE = [
    ("STM32F103C8T6", "STMicroelectronics", "MCU", "LQFP-48", "ARM Cortex-M3 MCU, 64 KB flash, 72 MHz",
     "https://www.st.com/resource/en/datasheet/stm32f103c8.pdf"),
    ("STM32G431CBU6", "STMicroelectronics", "MCU", "UFQFPN-48", "ARM Cortex-M4 MCU, 128 KB flash, 170 MHz", ""),
    ("ATmega328P-AU", "Microchip", "MCU", "TQFP-32", "8-bit AVR MCU, 32 KB flash", ""),
    ("ATtiny85-20PU", "Microchip", "MCU", "DIP-8", "Microcontrolador AVR de 8 bits, 8 KB flash", ""),
    ("RP2040", "Raspberry Pi", "MCU", "QFN-56", "Dual Cortex-M0+, 264 KB SRAM", ""),
    ("ESP32-WROOM-32E", "Espressif", "Module", "SMD module", "Wi-Fi + Bluetooth module, 4 MB flash", ""),
    ("ESP32-C3-MINI-1", "Espressif", "Module", "SMD module", "Модуль Wi-Fi и BLE, RISC-V", ""),
    ("NE555P", "Texas Instruments", "Timer", "DIP-8", "Precision timer", "https://www.ti.com/lit/ds/symlink/ne555.pdf"),
    ("LM358DR", "Texas Instruments", "Op-amp", "SOIC-8", "Dual operational amplifier", ""),
    ("LM324N", "Texas Instruments", "Op-amp", "DIP-14", "Amplificador operacional cuádruple", ""),
    ("MCP6002-I/SN", "Microchip", "Op-amp", "SOIC-8", "Rail-to-rail op-amp, 1 MHz", ""),
    ("74HC595D", "Nexperia", "Logic", "SOIC-16", "8-bit shift register with output latches", ""),
    ("74HCT245PW", "Nexperia", "Logic", "TSSOP-20", "Octal bus transceiver, level shifter 3.3 V to 5 V", ""),
    ("74HC00N", "Texas Instruments", "Logic", "DIP-14", "Четыре логических элемента 2И-НЕ", ""),
    ("CD4017BE", "Texas Instruments", "Logic", "DIP-16", "Decade counter/divider", ""),
    ("AMS1117-3.3", "AMS", "Regulator", "SOT-223", "LDO regulator 3.3 V 1 A", ""),
    ("LM7805CT", "STMicroelectronics", "Regulator", "TO-220", "Regulador lineal 5 V 1.5 A", ""),
    ("MP1584EN", "Monolithic Power", "Regulator", "SOIC-8", "Step-down converter 3 A, 28 V", ""),
    ("TL431AIDBZR", "Texas Instruments", "Voltage reference", "SOT-23", "Adjustable shunt reference", ""),
    ("2N2222A", "ON Semiconductor", "Transistor", "TO-92", "NPN transistor 40 V 0.8 A", ""),
    ("BC547B", "ON Semiconductor", "Transistor", "TO-92", "NPN транзистор общего назначения", ""),
    ("BC857C", "Nexperia", "Transistor", "SOT-23", "PNP general purpose transistor", ""),
    ("IRLZ44NPBF", "Infineon", "MOSFET", "TO-220", "N-channel logic-level MOSFET 55 V 47 A", ""),
    ("AO3400A", "Alpha & Omega", "MOSFET", "SOT-23", "MOSFET de canal N 30 V 5.7 A", ""),
    ("1N4148W", "Vishay", "Diode", "SOD-123", "Small signal fast switching diode", ""),
    ("1N5819", "Vishay", "Diode", "DO-41", "Schottky diode 40 V 1 A", ""),
    ("SS34", "Vishay", "Diode", "SMA", "Диод Шоттки 40 В 3 А", ""),
    ("BME280", "Bosch", "Sensor", "LGA-8", "Humidity, pressure and temperature sensor", ""),
    ("DS18B20+", "Analog Devices", "Sensor", "TO-92", "Sensor de temperatura 1-Wire", ""),
    ("INA219AIDCNR", "Texas Instruments", "Sensor", "SOT-23-8", "Current and power monitor, I2C", ""),
    ("INMP441", "TDK InvenSense", "Sensor", "Module", "I2S MEMS microphone", ""),
    ("B2B-XH-A", "JST", "Connector", "THT", "XH header 2 pins, 2.5 mm", ""),
    ("B4B-XH-A", "JST", "Connector", "THT", "Разъём XH 4 контакта, шаг 2.5 мм", ""),
    ("DF11-4DS-2C", "Hirose", "Connector", "Housing", "DF11 housing 4 positions", ""),
    ("WS2812B", "Worldsemi", "LED", "5050", "Addressable RGB LED", ""),
    ("LTST-C191KRKT", "Lite-On", "LED", "0603", "LED rojo 0603", ""),
    ("ABM8-16.000MHZ", "Abracon", "Crystal", "3.2x2.5 mm", "Crystal 16 MHz, 18 pF", ""),
    ("MF-MSMF050-2", "Bourns", "Fuse", "1812", "Самовосстанавливающийся предохранитель 0.5 А", ""),
]
MAKERS_PASSIVE = ["Yageo", "Vishay", "Murata", "Samsung", "KEMET", "Panasonic", "Bourns"]


def format_value(value: float, unit: str, prefixes=((1e6, "M"), (1e3, "k"), (1, ""))) -> str:
    """10000 -> "10k", 4700 -> "4.7k" (followed by the unit). Capacitors pass
    their own prefixes, so 1000 µF stays "1000µ" rather than "1m"."""
    for limit, prefix in prefixes:
        if value >= limit * 0.999:
            number = f"{value / limit:.3g}"
            return f"{number}{prefix}{unit}"
    return f"{value:g}{unit}"


def make_resistor(rng: random.Random) -> dict:
    ohms = rng.choice(E12) * 10 ** rng.randint(1, 5)
    package = rng.choice(SMD_PACKAGES + ["THT"])
    lang = rng.choice(["en", "en", "es", "ru"])
    value = format_value(ohms, "")
    # Russian writes kilo and mega in Cyrillic: 4.7k -> 4.7к
    shown = value.replace("k", "к").replace("M", "М") if lang == "ru" else value
    tolerance = rng.choice(["1%", "5%"])
    maker = rng.choice(MAKERS_PASSIVE[:2] + ["Bourns"])
    pn = f"RC{package if package != 'THT' else 'MF'}-{value.upper()}"
    return {
        "partNumber": pn,
        "manufacturer": maker,
        "category": "Resistor",
        "description": f"{RESISTOR[lang]} {shown} {OHM[lang]} {tolerance}",
        "package": package,
        "value": value,
        "unit": OHM["en"],
    }


def make_capacitor(rng: random.Random) -> dict:
    lang = rng.choice(["en", "en", "es", "ru"])
    if rng.random() < 0.75:
        farads = rng.choice([1.0, 2.2, 4.7, 10.0, 22.0, 47.0, 100.0]) * 10 ** rng.choice([-12, -9, -6])
        package = rng.choice(SMD_PACKAGES)
        voltage = rng.choice(["16V", "25V", "50V"])
        words = CAPACITOR[lang]
        maker = rng.choice(["Murata", "Samsung", "KEMET", "Yageo"])
    else:
        farads = rng.choice([10.0, 47.0, 100.0, 220.0, 470.0, 1000.0]) * 1e-6
        package = rng.choice(["Radial 5x11", "Radial 8x12", "SMD 6.3x5.4"])
        voltage = rng.choice(["16V", "25V", "35V"])
        words = ELECTROLYTIC[lang]
        maker = rng.choice(["Panasonic", "Nichicon", "Rubycon"])
    value = format_value(farads, "", ((1e-6, "µ"), (1e-9, "n"), (1e-12, "p")))
    prefix = "CC" + package if package in SMD_PACKAGES else "ECAP"
    return {
        "partNumber": f"{prefix}-{value}F-{voltage}",
        "manufacturer": maker,
        "category": "Capacitor",
        "description": f"{words} {value}F {voltage}",
        "package": package,
        "value": value,
        "unit": "F",
    }


def make_catalogue_part(rng: random.Random) -> dict:
    pn, maker, category, package, description, url = rng.choice(CATALOGUE)
    part = {"partNumber": pn, "manufacturer": maker, "category": category, "description": description,
            "package": package}
    if url:
        part["datasheetUrl"] = url
    return part


def make_part(rng: random.Random, bins: list) -> dict:
    roll = rng.random()
    part = make_resistor(rng) if roll < 0.35 else make_capacitor(rng) if roll < 0.6 else make_catalogue_part(rng)
    part["quantity"] = rng.choice([0, 1, 2, 5, 10, 25, 50, 100, 250, 1000, rng.randint(1, 500)])
    part["notes"] = MARKER + (" " + rng.choice(["Reel", "Cut tape", "From kit", "Para pruebas", "Из набора"])
                              if rng.random() < 0.3 else "")
    if bins and rng.random() < 0.9:  # some parts in no bin
        x, y = rng.choice(bins)
        part["x"], part["y"] = x, y
    return part


class Board:
    """The board's web API: form-encoded POSTs and JSON answers."""

    def __init__(self, host: str, password: str):
        self.base = "http://" + host
        handlers = []
        if password:
            # The board uses HTTP digest authentication, user "admin".
            manager = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            manager.add_password(None, self.base, "admin", password)
            handlers.append(urllib.request.HTTPDigestAuthHandler(manager))
        self.opener = urllib.request.build_opener(*handlers)

    def call(self, path: str, params: dict = None, post: bool = False) -> dict:
        data = urllib.parse.urlencode(params or {}).encode()
        url = self.base + path
        if not post and params:
            url += "?" + data.decode()
        request = urllib.request.Request(url, data=data if post else None)
        try:
            with self.opener.open(request, timeout=30) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as e:
            body = e.read().decode(errors="replace")
            raise SystemExit(f"{path}: HTTP {e.code} {body}")
        except urllib.error.URLError as e:
            raise SystemExit(f"Cannot reach {self.base}: {e.reason}")


def seed(board: Board, count: int, rng: random.Random) -> None:
    bins = [(b["x"], b["y"]) for b in board.call("/api/bins")["bins"] if not b.get("outside")]
    print(f"Adding {count} test parts over {len(bins)} bins...")
    start = time.time()
    for i in range(count):
        board.call("/api/part/add", make_part(rng, bins), post=True)
        if (i + 1) % 25 == 0 or i + 1 == count:
            rate = (i + 1) / (time.time() - start)
            print(f"  {i + 1}/{count}  ({rate:.1f} parts/s)", flush=True)


def clear(board: Board) -> None:
    # Search for the marker, delete what comes back, and repeat: each round
    # asks for the first page again, since deleting shifts the later ones.
    removed = 0
    while True:
        page = board.call("/api/parts", {"q": MARKER, "limit": 100})
        if not page["parts"]:
            break
        for part in page["parts"]:
            board.call("/api/part/delete", {"id": part["id"]}, post=True)
            removed += 1
        print(f"  removed {removed}, {page['total'] - len(page['parts'])} left", flush=True)
    print(f"Removed {removed} test parts.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default="smartbins.local", help="board address (default smartbins.local)")
    parser.add_argument("--count", type=int, default=300, help="parts to add (default 300)")
    parser.add_argument("--password", default="", help="web password, if one is set")
    parser.add_argument("--seed", type=int, default=None, help="random seed, for the same parts every time")
    parser.add_argument("--clear", action="store_true", help="remove the test parts instead of adding")
    args = parser.parse_args()

    board = Board(args.host, args.password)
    if args.clear:
        clear(board)
    else:
        seed(board, args.count, random.Random(args.seed))


if __name__ == "__main__":
    sys.exit(main())
