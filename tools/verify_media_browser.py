"""Verify the on-board four-row media list without creating or deleting TF files.

Exports the real RGB565 menu buffer, checks its CRC, visits page boundaries and
opens an existing photo/video. Serial keys use the physical-key state machine.
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
    parser.add_argument("--output", default="outputs/board-browser")
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    result = {"passed":False,"pages":[],"screenshots":[]}
    pending = b""
    thumbnails = []
    failures = re.compile(r"Guru Meditation|panic|watchdog|Brownout|state=ERROR|VIEW_FAIL|FORMAT_BEGIN|RECORD_START|storage: SAVED",re.I)
    with serial.Serial(args.port,115200,timeout=0.1) as port, (out/"serial.log").open("w") as log:
        def line(deadline):
            nonlocal pending
            while time.monotonic() < deadline:
                if b"\n" in pending:
                    raw,pending=pending.split(b"\n",1)
                    text=raw.decode(errors="replace").strip()
                    if not text.startswith("@"):
                        log.write(text+"\n")
                        log.flush()
                        if "STATS" not in text:
                            print(text,flush=True)
                    if failures.search(text):
                        raise RuntimeError(text)
                    thumb=re.search(r"THUMBNAIL name=(\S+) valid=(\d+) crc=([0-9a-f]+)",text)
                    if thumb:
                        thumbnails.append(thumb.groups())
                    return text
                pending+=port.read(max(1,port.in_waiting))
            raise TimeoutError("serial timeout")

        def wait(pattern,seconds=30):
            deadline=time.monotonic()+seconds
            regex=re.compile(pattern)
            while True:
                found=regex.search(line(deadline))
                if found:
                    return found

        def command(text):
            port.write(text.encode()+b"\n")

        def page():
            found=wait(r"BROWSER_PAGE first=(\d+) rows=(\d+) selected_row=(\d+) total=(\d+)")
            item=dict(zip(("first","rows","selected_row","total"),map(int,found.groups())))
            result["pages"].append(item)
            return item

        def status(expected):
            command("u")
            found=wait(r"UI_STATUS screen=(\d+) mode=(PHOTO|VIDEO) selected=(\S*) mounted=(\d+)")
            assert int(found.group(1)) == expected
            return found.group(3)

        def screenshot(name):
            command("t")
            start=wait(r"MENU_FRAME_BEGIN bytes=(\d+) crc=([0-9a-f]+) selected=(\S+)")
            data=bytearray()
            deadline=time.monotonic()+20
            while True:
                text=line(deadline)
                if text.startswith("@"):
                    data+=bytes.fromhex(text[1:])
                elif "MENU_FRAME_END" in text:
                    assert "valid=1" in text
                    break
            assert len(data) == int(start.group(1)) == 153600
            assert f"{zlib.crc32(data):08x}" == start.group(2)
            raw=out/(name+".rgb565")
            raw.write_bytes(data)
            png=out/(name+".png")
            subprocess.run(["ffmpeg","-v","error","-y","-f","rawvideo",
                            "-pixel_format","rgb565be","-video_size","320x240",
                            "-i",str(raw),"-frames:v","1",str(png)],check=True)
            result["screenshots"].append({"file":str(png),"crc":start.group(2),"selected":start.group(3)})
            return data

        def stats():
            values=wait(r"STATS .*internal_free=(\d+) psram_free=(\d+)")
            return tuple(map(int,values.groups()))

        try:
            port.dtr=False
            HardReset(port,uses_usb=True)()
            result["filesystem"]=wait(r"TF mounted .*fs=(exFAT|FAT32)\b").group(1)
            wait(r"FIRST_FRAME")
            result["memory_before"]=stats()
            status(0)
            command("n")
            initial=page()
            wait(r"BROWSE count=\d+ selected=\S+\.BMP")
            assert initial["first"] == 1 and initial["selected_row"] == 0
            assert initial["total"] >= 8, "test requires at least two existing pages"
            status(3)
            result["memory_menu"]=stats()
            screenshot("photos")
            # Within one page only the highlight should change, with no thumbnail reads.
            first_load=len(thumbnails)
            for row in (1,2,3):
                command("n")
                current=page()
                wait(r"BROWSE count=")
                assert current["first"] == 1 and current["selected_row"] == row
                assert len(thumbnails) == first_load, "same-page thumbnail cache was not reused"
            command("n")
            current=page()
            wait(r"BROWSE count=")
            assert current["first"] == 5 and current["selected_row"] == 0
            assert len(thumbnails) == first_load+4
            screenshot("second-page")
            # Move back to the first item and wrap to the last page.
            for _ in range(5):
                command("l")
                current=page()
                wait(r"BROWSE count=")
            assert current["first"]+current["selected_row"] == current["total"]
            selected=status(3)
            assert selected.endswith(".AVI")
            screenshot("videos")
            command("e")
            wait(r"PLAY_START")
            wait(r"VIEW_OPEN .*type=AVI")
            status(5)
            command("b")
            status(3)
            screenshot("after-playback")
            command("b")
            status(0)
            result["memory_after"]=stats()
            # After a warmup, re-enter and leave the list three times: no growing allocations.
            idle=[]
            for _ in range(3):
                command("n")
                page()
                wait(r"BROWSE count=")
                status(3)
                command("b")
                status(0)
                idle.append(stats())
            result["memory_idle_cycles"]=idle
            assert len(set(idle)) == 1, "menu allocations did not return to a stable baseline"
            # Final leave the board displaying the new first-page list.
            command("n")
            page()
            wait(r"BROWSE count=")
            command("e")
            wait(r"VIEW_OPEN .*type=BMP")
            status(4)
            command("b")
            status(3)
            screenshot("final-menu")
            result["thumbnail_reads"]=[dict(zip(("file","valid","crc"),item)) for item in thumbnails]
            assert all(item[1] == "1" for item in thumbnails)
            assert any(item[0].endswith(".BMP") for item in thumbnails)
            assert any(item[0].endswith(".AVI") for item in thumbnails)
            result["passed"]=True
        except Exception as error:
            result["error"]=str(error)
            print("FAIL:",error,flush=True)
        finally:
            (out/"summary.json").write_text(json.dumps(result,indent=2,ensure_ascii=False))
    print(json.dumps({k:v for k,v in result.items() if k!="thumbnail_reads"},indent=2),flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
