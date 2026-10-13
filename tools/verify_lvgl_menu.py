"""Read-only board validation for LVGL menus, TF usage and serial format blocking.

Exports the actual displayed RGB565 frame and checks CRC. Never formats,
records, creates or deletes TF files; resets to leave the locked confirmation
page because only physical keys can cancel it. Final state is the root menu.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
import zlib

import serial
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--output", default="outputs/lvgl-board-menu")
    parser.add_argument("--cycles", type=int, default=4)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("cycles must be positive")
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "screenshots": [], "statuses": []}
    pending = b""
    faults = re.compile(r"Guru Meditation|panic|watchdog|Brownout|state=ERROR|FORMAT_BEGIN|RECORD_START|storage: SAVED", re.I)

    with serial.Serial(args.port, 115200, timeout=0.1) as port, (out / "serial.log").open("w") as log:
        def line(deadline):
            nonlocal pending
            while time.monotonic() < deadline:
                if b"\n" in pending:
                    raw, pending = pending.split(b"\n", 1)
                    text = raw.decode(errors="replace").strip()
                    if not text.startswith("@"):
                        log.write(text + "\n")
                        log.flush()
                        if "STATS" not in text:
                            print(text, flush=True)
                        if faults.search(text):
                            raise RuntimeError(text)
                    return text
                pending += port.read(max(1, port.in_waiting))
            raise TimeoutError("serial response timeout")

        def wait(pattern, seconds=30):
            deadline = time.monotonic() + seconds
            while True:
                found = re.search(pattern, line(deadline))
                if found:
                    return found

        def command(text):
            port.write(text.encode() + b"\n")

        def status(expected):
            command("u")
            found = wait(r"UI_STATUS screen=(\d+) mode=(PHOTO|VIDEO) selected=(\S*) mounted=(\d+) unsupported=(\d+) yes=(\d+) streaming=(\d+) captures=(\d+)")
            item = dict(zip(("screen", "mode", "selected", "mounted", "unsupported", "yes", "streaming", "captures"), found.groups()))
            assert int(item["screen"]) == expected, item
            assert int(item["streaming"]) == (1 if expected == 0 else 0), item
            result["statuses"].append(item)
            return item

        def reset():
            nonlocal pending
            port.dtr = False
            port.reset_input_buffer()
            pending = b""
            HardReset(port, uses_usb=True)()
            wait(r"camera-storage v2\.9-menu-keys state=INITIALIZING")
            wait(r"TF mounted .*fs=(exFAT|FAT32)")
            wait(r"FIRST_FRAME")
            status(0)

        def screenshot(name):
            # Each command/status is consumed on separate application frames,
            # so the previous dirty page has reached the synchronous LCD output.
            command("t")
            start = wait(r"MENU_FRAME_BEGIN bytes=(\d+) crc=([0-9a-f]+)")
            data = bytearray()
            deadline = time.monotonic() + 30
            while True:
                text = line(deadline)
                if text.startswith("@"):
                    data.extend(bytes.fromhex(text[1:]))
                elif "MENU_FRAME_END" in text:
                    assert "valid=1" in text
                    break
            assert len(data) == int(start.group(1)) == 153600
            assert f"{zlib.crc32(data):08x}" == start.group(2)
            raw = out / f"{name}.rgb565"
            raw.write_bytes(data)
            png = raw.with_suffix(".png")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo",
                            "-pixel_format", "rgb565be", "-video_size", "320x240",
                            "-i", str(raw), "-frames:v", "1", str(png)], check=True)
            result["screenshots"].append({"file": str(png), "crc": start.group(2)})
            return bytes(data)

        def memory():
            found = wait(r"STATS .*internal_free=(\d+) psram_free=(\d+)")
            return tuple(map(int, found.groups()))

        try:
            reset()
            command("n")
            entered = status(6)
            # A menu must stop actual acquisition; changing image CRC is insufficient.
            for _ in range(2):
                idle = wait(r"MENU_IDLE streaming=(\d+) captures=(\d+) poll_ms=(\d+)")
                assert idle.group(1) == "0" and idle.group(2) == entered["captures"]
                assert idle.group(3) == "10"
            result["camera_idle_verified"] = True
            screenshot("functions")
            command("n")
            status(6)
            command("e")
            status(7)
            screenshot("tf-card")
            command("e")
            assert status(8)["mounted"] == "1"
            screenshot("card-usage")
            command("b")
            status(7)
            command("n")
            status(7)
            command("e")
            assert status(1)["yes"] == "0"
            before = screenshot("format-first-cancel")
            # Serial navigation and confirmations must not select YES or erase.
            for key in ("n", "e", "n", "e", "b"):
                command(key)
                assert status(1)["yes"] == "0"
            assert screenshot("format-serial-blocked") == before
            result["serial_format_blocked"] = True
            reset()
            idle = []
            transitions = []
            for cycle in range(args.cycles):
                command("n")
                status(6)
                command("n")
                status(6)
                command("e")
                status(7)
                command("e")
                status(8)
                command("b")
                status(7)
                command("b")
                status(6)
                # TF returns to root with row 1 selected; down selects BACK.
                command("n")
                status(6)
                command("e")
                wait(r"PREVIEW_RESUME_BLACK shown warmup_frames=3")
                wait(r"CAMERA_RESUMED streaming=1")
                for expected in range(1, 4):
                    stage = wait(r"PREVIEW_WARMUP discarded=(\d+) buffer=(\d+) valid=(\d+)")
                    assert int(stage.group(1)) == expected
                shown = wait(r"PREVIEW_RESUME_SHOWN warmup_frames=3 black_us=(\d+)")
                transitions.append(int(shown.group(1)))
                status(0)
                idle.append(memory())
            assert len(set(idle)) == 1, idle
            result["memory_idle_cycles"] = idle
            result["resume_black_us"] = transitions
            command("n")
            status(6)
            screenshot("final-functions")
            result["passed"] = True
        except Exception as error:
            result["error"] = str(error)
            print("FAIL:", error, flush=True)
        finally:
            (out / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False))
    print(json.dumps(result, indent=2, ensure_ascii=False), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
