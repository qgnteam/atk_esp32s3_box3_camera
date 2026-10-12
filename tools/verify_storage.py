"""Run real exFAT photo/video writes and full file readback through USB serial.

Creates new numbered files on the card; never formats or deletes prior files.
The serial p/r/v commands exercise the same storage path as the BOOT button.
"""
import argparse
from collections import deque
import json
from pathlib import Path
import re
import sys
import time
import serial
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--record-seconds", type=float, default=6)
    parser.add_argument("--log", default="outputs/storage-exfat.log")
    parser.add_argument("--allow-fat", action="store_true", help="Verify storage functionality on FAT; does not qualify as exFAT testing")
    args = parser.parse_args()
    if not 1 <= args.record_seconds <= 65:
        parser.error("record duration must be between 1 and 65 seconds")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    path = Path(args.log)
    path.parent.mkdir(parents=True, exist_ok=True)
    results = {"passed": False, "photos": [], "video": None, "checks": []}
    pending = b""
    queued = deque()
    failures = re.compile(r"Guru Meditation|panic|watchdog|Brownout|state=ERROR|save failed|FILE_VERIFY open failed|TF mount failed|cannot create", re.I)

    with serial.Serial(args.port, 115200, timeout=0.1) as port, path.open("w", encoding="utf-8") as log:
        def read_lines():
            nonlocal pending
            pending += port.read(max(1, port.in_waiting))
            lines = []
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                log.write(line + "\n")
                log.flush()
                if "STATS" not in line:
                    print(line, flush=True)
                if failures.search(line):
                    raise RuntimeError(line)
                lines.append(line)
                queued.append(line)
            return lines

        def wait_for(pattern, seconds=20):
            deadline = time.monotonic() + seconds
            regex = re.compile(pattern)
            while time.monotonic() < deadline:
                if not queued:
                    read_lines()
                while queued:
                    line = queued.popleft()
                    match = regex.search(line)
                    if match:
                        return match
            raise RuntimeError("timeout waiting for " + pattern)

        def photo():
            port.write(b"p\n")
            name = wait_for(r"SAVED (\S+\.BMP)").group(1)
            port.write(b"v\n")
            checked = wait_for(r"FILE_VERIFY path=(\S+) bytes=(\d+) expected=(\d+) crc=([0-9a-f]+) valid=(\d+)")
            assert checked.group(1) == name and checked.group(2) == "230454"
            assert checked.group(2) == checked.group(3) and checked.group(5) == "1"
            results["photos"].append(name)
            results["checks"].append(checked.group(0))

        try:
            port.dtr = False
            HardReset(port, uses_usb=True)()
            filesystem = wait_for(r"TF mounted .*fs=(exFAT|FAT)\b").group(1)
            results["filesystem"] = filesystem
            if filesystem != "exFAT" and not args.allow_fat:
                raise RuntimeError("card is FAT, not exFAT; card was not formatted")
            wait_for(r"FIRST_FRAME")
            photo()
            port.write(b"r\n")
            wait_for(r"RECORD_START")
            deadline = time.monotonic() + args.record_seconds
            while time.monotonic() < deadline:
                read_lines()
            if args.record_seconds < 60:
                port.write(b"r\n")
            stopped = wait_for(r"RECORD_STOP frames=(\d+) period_us=(\d+)", 25)
            frames, period = map(int, stopped.groups())
            assert 1 <= frames <= 300 and period > 0
            name = wait_for(r"SAVED (\S+\.AVI)").group(1)
            results["video"] = {"path": name, "frames": frames, "period_us": period}
            port.write(b"v\n")
            checked = wait_for(r"FILE_VERIFY path=(\S+) bytes=(\d+) expected=(\d+) crc=([0-9a-f]+) valid=(\d+)", 45)
            assert checked.group(1) == name and checked.group(5) == "1"
            assert int(checked.group(2)) == int(checked.group(3)) == 232 + frames*230424
            results["checks"].append(checked.group(0))
            photo()
            assert len(set(results["photos"])) == 2
            results["passed"] = True
        except Exception as error:
            results["error"] = str(error)
            print("FAIL: " + str(error), flush=True)
        finally:
            # A failure must not leave a successfully started recorder running.
            # The fixed duration limit also closes it if the serial link is lost.
            path.with_suffix(".json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(results, ensure_ascii=False, indent=2), flush=True)
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
