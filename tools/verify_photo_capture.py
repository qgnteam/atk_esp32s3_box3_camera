"""Verify real VGA capture, QVGA recovery, file readback and notice timing.

Creates three photos by default; does not format or delete TF files. Serial e
uses the same UI action as K2; visual feedback still needs human observation.
"""
import argparse
import json
from pathlib import Path
import re
import time

import serial
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--output", default="outputs/photo-capture")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("count must be positive")
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    result = {"passed": False, "photos": []}
    pending = b""
    errors = re.compile(r"Guru Meditation|panic|watchdog|Brownout|state=ERROR|RECOVERING|photo capture failed|save failed", re.I)
    with serial.Serial(args.port, 115200, timeout=0.1) as port, (out / "serial.log").open("w") as log:
        def wait(pattern, seconds=30):
            nonlocal pending
            deadline = time.monotonic() + seconds
            expression = re.compile(pattern)
            while time.monotonic() < deadline:
                if b"\n" not in pending:
                    pending += port.read(max(1, port.in_waiting))
                    continue
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                log.write(line + "\n")
                log.flush()
                if "STATS" not in line:
                    print(line, flush=True)
                if errors.search(line):
                    raise RuntimeError(line)
                match = expression.search(line)
                if match:
                    return match
            raise TimeoutError(pattern)

        try:
            port.dtr = False
            HardReset(port, uses_usb=True)()
            result["filesystem"] = wait(r"TF mounted .*fs=(exFAT|FAT32)\b").group(1)
            wait(r"FIRST_FRAME")
            for _ in range(args.count):
                port.write(b"e\n")
                wait(r"PHOTO_FEEDBACK requested")
                wait(r"camera: format=.*width=640 height=480 stride=1280")
                name = wait(r"storage: SAVED (\S+\.BMP)").group(1)
                feedback = int(wait(r"PHOTO_FEEDBACK complete elapsed_us=(\d+)").group(1))
                assert feedback >= 500000, feedback
                wait(r"camera: format=.*width=320 height=240 stride=640")
                wait(r"PHOTO_NOTICE shown hold_us=3000000")
                elapsed = int(wait(r"PHOTO_NOTICE expired elapsed_us=(\d+)").group(1))
                assert 3000000 <= elapsed < 3200000, elapsed
                # Check publication length and a full-file readback after the timer.
                port.write(b"v\n")
                checked = wait(r"FILE_VERIFY path=(\S+) bytes=(\d+) expected=(\d+) crc=([0-9a-f]+) valid=(\d+)")
                assert checked.group(1) == name
                assert checked.group(2) == checked.group(3) == "921654"
                assert checked.group(5) == "1"
                port.write(b"u\n")
                wait(r"UI_STATUS screen=0 mode=PHOTO .*mounted=1")
                result["photos"].append({"path": name, "notice_us": elapsed, "feedback_us": feedback,
                                         "bytes": int(checked.group(2)), "crc": checked.group(4)})
            assert len({photo["path"] for photo in result["photos"]}) == args.count
            result["passed"] = True
        except Exception as error:
            result["error"] = str(error)
        finally:
            (out / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False))
    print(json.dumps(result, indent=2, ensure_ascii=False), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
