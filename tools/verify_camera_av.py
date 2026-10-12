"""Non-destructive board UI/audio test. Creates files; never formats the card.

Exports actual BMP/AVI bytes, checks RIFF indexes and PCM samples, then plays
both media types using the same menu reducer as the physical buttons.
"""
import argparse
import array
import json
from pathlib import Path
import re
import math
import subprocess
import time
import zlib
import serial
from esptool.reset import HardReset


def u32(data, offset):
    return int.from_bytes(data[offset:offset+4], "little")


def inspect_avi(data):
    assert data[:4] == b"RIFF" and data[8:12] == b"AVI "
    assert u32(data, 4)+8 == len(data) and u32(data, 56) == 2
    assert data[232:236] == b"auds"
    assert (u32(data,252),u32(data,256),u32(data,276)) == (2,48000,2)
    assert data[296:300] == b"\x01\x00\x01\x00"
    assert u32(data,300) == 24000 and data[308:312] == b"\x02\x00\x10\x00"
    assert data[320:324] == b"movi"
    cursor = 324
    end = 320+u32(data,316)
    offsets, pcm, frames = [], bytearray(), 0
    while cursor < end:
        tag, size = data[cursor:cursor+4], u32(data,cursor+4)
        assert tag in (b"00db", b"01wb") and size % 2 == 0
        assert cursor+8+size <= end
        offsets.append((tag, cursor-320, size))
        if tag == b"00db":
            assert size == 230400
            frames += 1
        else:
            pcm += data[cursor+8:cursor+8+size]
        cursor += 8+size
    assert cursor == end and data[end:end+4] == b"idx1"
    assert u32(data,end+4) == len(offsets)*16
    assert end+8+len(offsets)*16 == len(data)
    for item, (tag, offset, size) in enumerate(offsets):
        entry = end+8+item*16
        assert data[entry:entry+4] == tag
        assert u32(data,entry+8) == offset and u32(data,entry+12) == size
    assert frames == u32(data,48) and len(pcm)//2 == u32(data,264)
    samples = array.array("h", pcm)
    peak = max(abs(value) for value in samples)
    rms = math.sqrt(sum(value*value for value in samples)/len(samples))
    assert peak > 0 and rms > 0, "microphone produced only zeros"
    video_duration = frames*u32(data,32)/1e6
    duration = len(samples)/24000
    assert abs(duration-video_duration) < 0.02
    return {"frames":frames,"pcm_samples":len(samples),"duration_seconds":duration,
            "peak":peak,"rms":rms,"distinct_samples":len(set(samples)),
            "video_duration_seconds":video_duration,"chunks":len(offsets)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--seconds", type=float, default=4)
    parser.add_argument("--output", default="outputs/board-av")
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    results = {"passed":False}
    pending = b""
    queue = []
    failures = re.compile(r"Guru Meditation|panic|watchdog|Brownout|state=ERROR|save failed|VIEW_FAIL|overflows=[1-9]|FORMAT_BEGIN", re.I)
    with serial.Serial(args.port, 115200, timeout=0.1) as port, (out/"serial.log").open("w") as log:
        def read_line(deadline):
            nonlocal pending
            while time.monotonic() < deadline:
                if b"\n" in pending:
                    raw,pending = pending.split(b"\n",1)
                    line = raw.decode("utf-8", errors="replace").strip()
                    if not line.startswith("@"):
                        log.write(line+"\n")
                        log.flush()
                        if "STATS" not in line:
                            print(line,flush=True)
                    if failures.search(line):
                        raise RuntimeError(line)
                    return line
                pending += port.read(max(1,port.in_waiting))
            raise TimeoutError("serial line timeout")

        def wait_for(pattern, timeout=30):
            deadline = time.monotonic()+timeout
            expression = re.compile(pattern)
            while time.monotonic() < deadline:
                line = read_line(deadline)
                match = expression.search(line)
                if match:
                    return match
            raise TimeoutError(pattern)

        def command(text):
            port.write(text.encode()+b"\n")

        def status(screen, mode=None):
            command("u")
            match = wait_for(r"UI_STATUS screen=(\d+) mode=(PHOTO|VIDEO)")
            assert int(match.group(1)) == screen, match.group(0)
            if mode:
                assert match.group(2) == mode

        def export_file(target, expected_crc):
            command("d")
            header = wait_for(r"EXPORT_BEGIN path=(\S+) bytes=(\d+)")
            size = int(header.group(2))
            data = bytearray()
            deadline = time.monotonic()+120
            while True:
                line = read_line(deadline)
                if line.startswith("@"):
                    data += bytes.fromhex(line[1:])
                elif "EXPORT_END" in line:
                    assert "valid=1" in line
                    break
            assert len(data) == size
            assert f"{zlib.crc32(data):08x}" == expected_crc
            target.write_bytes(data)
            return data

        try:
            port.dtr = False
            HardReset(port,uses_usb=True)()
            results["filesystem"] = wait_for(r"TF mounted .*fs=(exFAT|FAT32)\b").group(1)
            wait_for(r"FIRST_FRAME")
            status(0,"PHOTO")
            command("e")
            results["photo"] = wait_for(r"SAVED (\S+\.BMP)").group(1)
            command("v")
            checked = wait_for(r"FILE_VERIFY .*bytes=(\d+) expected=(\d+) crc=([0-9a-f]+) valid=(\d+)")
            assert checked.group(1) == checked.group(2) == "230454" and checked.group(4) == "1"
            bmp = export_file(out/"photo.bmp",checked.group(3))
            assert bmp[:2] == b"BM" and u32(bmp,2) == len(bmp)
            command("l")
            status(0,"VIDEO")
            command("e")
            wait_for(r"RECORD_START")
            deadline = time.monotonic()+args.seconds
            while time.monotonic() < deadline:
                try:
                    read_line(deadline)
                except TimeoutError:
                    break
            command("e")
            stopped = wait_for(r"RECORD_STOP frames=(\d+) period_us=(\d+) audio_bytes=(\d+) chunks=(\d+) audio_valid=(\d+)")
            assert stopped.group(5) == "1"
            results["video"] = wait_for(r"SAVED (\S+\.AVI)").group(1)
            command("v")
            checked = wait_for(r"FILE_VERIFY .*bytes=(\d+) expected=(\d+) crc=([0-9a-f]+) valid=(\d+)",60)
            assert checked.group(1) == checked.group(2) and checked.group(4) == "1"
            avi = export_file(out/"video.avi",checked.group(3))
            results["avi"] = inspect_avi(avi)
            # First list item is the oldest photo; previous wraps to newest video.
            command("n")
            wait_for(r"BROWSE count=\d+ selected=\S+\.BMP")
            status(3)
            command("e")
            wait_for(r"VIEW_OPEN .*type=BMP")
            status(4)
            command("b")
            status(3)
            command("l")
            wait_for(r"BROWSE .*selected=\S+\.AVI")
            command("e")
            wait_for(r"PLAY_START .*audio_bytes=[1-9]")
            wait_for(r"VIEW_OPEN .*type=AVI")
            status(5)
            wait_for(r"PLAY_STOP complete",args.seconds+15)
            status(3)
            command("b")
            status(0,"VIDEO")
            command("l")
            status(0,"PHOTO")
            # An independent decoder must recognise and decode both streams.
            probe = subprocess.run(["ffprobe","-v","error","-show_streams","-show_format",
                "-of","json",str(out/"video.avi")],capture_output=True,text=True,check=True)
            results["ffprobe"] = json.loads(probe.stdout)
            codecs = [s["codec_name"] for s in results["ffprobe"]["streams"]]
            assert codecs == ["rawvideo","pcm_s16le"], codecs
            subprocess.run(["ffmpeg","-v","error","-i",str(out/"video.avi"),
                "-map","0:v","-map","0:a","-f","null","-"],check=True)
            subprocess.run(["ffmpeg","-v","error","-i",str(out/"photo.bmp"),
                "-f","null","-"],check=True)
            results["passed"] = True
        except Exception as error:
            results["error"] = str(error)
            print("FAIL:",error,flush=True)
        finally:
            # Reset after a failed test stops any recorder/playback, never formats.
            if not results["passed"]:
                HardReset(port,uses_usb=True)()
            (out/"summary.json").write_text(json.dumps(results,indent=2,ensure_ascii=False))
    print(json.dumps(results,indent=2,ensure_ascii=False),flush=True)
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
