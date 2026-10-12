"""有界串口日志采集；复位设备并统计当前版本的预览运行结果。"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="COM4")
parser.add_argument("--seconds", type=float, default=30)
parser.add_argument("--log", default="outputs/serial.log")
parser.add_argument("--reset", action="store_true")
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
path = Path(args.log)
path.parent.mkdir(parents=True, exist_ok=True)
stats = []
errors = []
first_frames = []
pattern = re.compile(r"STATS fps=([\d.]+) display_ms=([\d.]+) total=(\d+) dropped=(\d+) crc=([0-9a-f]+) internal_free=(\d+) psram_free=(\d+)")

# USB JTAG 复位后保留端口，按整行解码避免拆分 UTF-8 字节。
with serial.Serial(args.port, 115200, timeout=0.1) as port, path.open("w", encoding="utf-8") as log:
    if args.reset:
        port.dtr = False
        HardReset(port, uses_usb=True)()
    started = time.monotonic()
    pending = b""
    while time.monotonic() - started < args.seconds:
        data = port.read(max(1, port.in_waiting))
        if not data:
            continue
        pending += data
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            log.write(line + "\n")
            log.flush()
            # 长时验证每 30 个样本输出一次，完整原始日志全部保存。
            match = pattern.search(line)
            if match:
                fps, display_ms, total, dropped, crc, internal, psram = match.groups()
                stats.append(dict(fps=float(fps), display_ms=float(display_ms),
                                  total=int(total), dropped=int(dropped), crc=crc,
                                  internal_free=int(internal), psram_free=int(psram)))
                if len(stats) % 30 == 1:
                    print(line, flush=True)
            else:
                print(line, flush=True)
            first = re.search(r"FIRST_FRAME startup_ms=(\d+)", line)
            if first:
                first_frames.append(int(first.group(1)))
            if re.search(r"state=ERROR|capture error|RECOVERING|Guru Meditation|panic|watchdog|Brownout", line, re.I):
                errors.append(line)
    if pending:
        log.write(pending.decode("utf-8", errors="replace"))

# 缺少统计不能算成功；首帧列表用于识别意外重启，但续采允许没有首帧。
summary = dict(duration_seconds=args.seconds, samples=len(stats),
               first_frame_ms=first_frames, errors=errors)
if stats:
    summary.update(fps_min=min(s["fps"] for s in stats),
                   fps_max=max(s["fps"] for s in stats),
                   fps_mean=sum(s["fps"] for s in stats) / len(stats),
                   display_ms_mean=sum(s["display_ms"] for s in stats) / len(stats),
                   total_start=stats[0]["total"], total_end=stats[-1]["total"],
                   dropped_end=stats[-1]["dropped"],
                   distinct_crc=len({s["crc"] for s in stats}),
                   internal_free_min=min(s["internal_free"] for s in stats),
                   internal_free_max=max(s["internal_free"] for s in stats),
                   psram_free_min=min(s["psram_free"] for s in stats),
                   psram_free_max=max(s["psram_free"] for s in stats))
path.with_suffix(".json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
sys.exit(0 if stats and not errors else 1)
