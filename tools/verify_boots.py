"""重复 USB 软件复位并检查当前固件首帧；这不是断电冷启动测试。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="COM4")
parser.add_argument("--cycles", type=int, default=20)
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
directory = Path("outputs/boot-cycles")
directory.mkdir(parents=True, exist_ok=True)
results = []

# 每次采集 4 秒，必须只有一个首帧且稳定样本不低于 15 FPS。
for cycle in range(1, args.cycles + 1):
    log = directory / f"boot-{cycle:02d}.log"
    process = subprocess.run(
        [sys.executable, str(Path(__file__).with_name("serial_verify.py")),
         "--port", args.port, "--seconds", "4", "--reset", "--log", str(log)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", timeout=15)
    capture = log.with_suffix(".json")
    detail = json.loads(capture.read_text(encoding="utf-8")) if capture.exists() else {}
    first = detail.get("first_frame_ms", [])
    passed = (process.returncode == 0 and len(first) == 1 and first[0] <= 3000
              and detail.get("fps_min", 0) >= 15)
    results.append(dict(cycle=cycle, passed=passed, detail=detail))
    print(f"USB reset cycle {cycle}/{args.cycles}: {'PASS' if passed else 'FAIL'} first_ms={first}", flush=True)
    if not passed:
        print(process.stdout, flush=True)
        break

summary = dict(requested_cycles=args.cycles, completed_cycles=len(results),
               passed_cycles=sum(r["passed"] for r in results), results=results)
(directory / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
sys.exit(0 if len(results) == args.cycles and all(r["passed"] for r in results) else 1)
