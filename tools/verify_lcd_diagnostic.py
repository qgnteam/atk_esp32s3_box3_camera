"""Verify actual LCD DMA source colours/CRC and hold a physical test screen.

Does not read LCD GRAM or infer panel appearance. Does not write TF files.
--reset starts a fresh boot; --sweep checks all primary colours plus white/black.
The final colour remains visible for human inspection, with optional 180-degree
rotation. Lower CONFIG_BOX3_LCD_SPI_HZ separately for a signal timing comparison.
"""
import argparse
from pathlib import Path
import json
import re
import time
import zlib

import serial
from esptool.reset import HardReset

COLORS = {"blue": ("B", 0x001f), "white": ("W", 0xffff),
          "red": ("R", 0xf800), "green": ("G", 0x07e0), "black": ("K", 0)}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", default="/dev/ttyACM0")
parser.add_argument("--output", default="outputs/lcd-diagnostic")
parser.add_argument("--color", choices=COLORS, default="white")
parser.add_argument("--reset", action="store_true")
parser.add_argument("--sweep", action="store_true")
parser.add_argument("--rotate", action="store_true")
args = parser.parse_args()
out = Path(args.output)
out.mkdir(parents=True, exist_ok=True)
result = {"passed": False, "colour_checks": [], "physical_appearance_verified": False}
pending = b""
with serial.Serial(args.port, 115200, timeout=0.1) as port, (out / "serial.log").open("w") as log:
    def line(deadline):
        global pending
        while time.monotonic() < deadline:
            if b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                log.write(text + "\n")
                log.flush()
                if re.search(r"Guru Meditation|panic|watchdog|Brownout|state=ERROR|FORMAT_BEGIN|RECORD_START|storage: SAVED", text, re.I):
                    raise RuntimeError(text)
                if "STATS" not in text:
                    print(text, flush=True)
                return text
            pending += port.read(max(1, port.in_waiting))
        raise TimeoutError("LCD test response timeout")

    def wait(pattern):
        deadline = time.monotonic() + 15
        while True:
            match = re.search(pattern, line(deadline))
            if match:
                return match

    def draw(command, color, rotated=False):
        port.write(command.encode() + b"\n")
        expected_crc = zlib.crc32(color.to_bytes(2, "big") * (320 * 40))
        for y in range(0, 240, 40):
            stripe = wait(r"LCD_TEST_STRIP y=(\d+) rows=(\d+) color=([0-9a-f]+) crc=([0-9a-f]+) uniform=(\d+)")
            actual_y, rows, value, crc, uniform = stripe.groups()
            assert int(actual_y) == y and int(rows) == 40
            assert int(value, 16) == color and int(crc, 16) == expected_crc and uniform == "1"
        end = wait(r"LCD_TEST_END color=([0-9a-f]+) reversed=(\d+) rows=(\d+) spi_hz=(\d+) result=(\S+)")
        assert int(end.group(1), 16) == color and int(end.group(2)) == int(rotated)
        assert int(end.group(3)) == 240 and end.group(5) == "ESP_OK"
        result["colour_checks"].append({"command": command, "rgb565": f"{color:04x}",
                                       "crc_per_strip": f"{expected_crc:08x}", "reversed": rotated,
                                       "configured_spi_hz": int(end.group(4)), "pixels_checked": 76800})

    try:
        if args.reset:
            port.dtr = False
            port.reset_input_buffer()
            HardReset(port, uses_usb=True)()
            wait(r"state=INITIALIZING")
            wait(r"FIRST_FRAME")
        if args.sweep:
            for command, color in COLORS.values():
                draw(command, color)
        command, color = COLORS[args.color]
        draw(command, color)
        if args.rotate:
            draw("X", color, True)
        # Normal capture/record controls must be ignored while the screen is held.
        port.write(b"p\nr\nu\n")
        state = wait(r"LCD_TEST_STATUS color=([0-9a-f]+) reversed=(\d+)")
        assert int(state.group(1), 16) == color and int(state.group(2)) == int(args.rotate)
        result["held_colour"] = args.color
        result["passed"] = True
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        (out / "summary.json").write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
