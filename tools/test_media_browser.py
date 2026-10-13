"""Run actual C pagination, thumbnail decoding and list rendering on the host.

All fixtures and render artifacts are created inside ignored outputs. Tests
never open /dev devices or alter the board's TF card.
"""
import ctypes
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "outputs/browser-tests"
OUT.mkdir(parents=True, exist_ok=True)
subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-O1",
                "-I", str(ROOT / "main"),
                *(str(ROOT / name) for name in (
                    "main/storage/media_browser.c", "main/storage/media_thumbnail.c",
                    "main/storage/media_format.c", "main/ui/media_list.c",
                    "main/display/lcd_font.c")),
                "-o", str(OUT / "browser.so")], check=True)
lib = ctypes.CDLL(str(OUT / "browser.so"))
Byte = ctypes.c_uint8
Pointer = ctypes.POINTER(Byte)


class Page(ctypes.Structure):
    _fields_ = [("names", (ctypes.c_char * 32) * 4),
                ("count", ctypes.c_uint), ("selected_row", ctypes.c_uint),
                ("selected_index", ctypes.c_uint), ("file_count", ctypes.c_uint)]


class View(ctypes.Structure):
    _fields_ = [("page", Page), ("thumbnails", Pointer * 4),
                ("valid", ctypes.c_bool * 4), ("notice", ctypes.c_char_p)]


lib.MediaBrowserReadPage.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(Page)]
lib.MediaBrowserReadPage.restype = ctypes.c_bool
lib.MediaThumbnailRead.argtypes = [ctypes.c_char_p, Pointer, Pointer, ctypes.c_size_t]
lib.MediaThumbnailRead.restype = ctypes.c_bool
lib.MediaListRender.argtypes = [ctypes.POINTER(View), Pointer]
lib.MediaListRender.restype = None
lib.MediaBmpHeader.argtypes = [Pointer]
lib.MediaAvHeader.argtypes = [Pointer] + [ctypes.c_uint32] * 5
lib.MediaAviHeader.argtypes = [Pointer, ctypes.c_uint32, ctypes.c_uint32]
lib.LcdFontDraw.argtypes = [Pointer, ctypes.c_uint, ctypes.c_uint, ctypes.c_int,
                           ctypes.c_int, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint16]

results = {}
with tempfile.TemporaryDirectory(dir=OUT) as directory:
    folder = Path(directory)
    names = [f"IMG{item:05d}.BMP" for item in range(137)]
    names += [f"VID{item:05d}.AVI" for item in range(83)]
    shuffled = list(names)
    random.Random(47).shuffle(shuffled)
    for name in shuffled:
        (folder / name).write_bytes(b"fixture")
    # Files excluded by the firmware's browsing rules must never enter a page.
    for name in ("IMG00001.TMP", ".hidden.BMP", "lower.bmp", "NOTES.TXT", "A" * 32 + ".AVI"):
        (folder / name).write_bytes(b"excluded")
    (folder / "DIR00001.BMP").mkdir()
    for index, name in enumerate(names):
        page = Page()
        assert lib.MediaBrowserReadPage(str(folder).encode(), name.encode(), ctypes.byref(page))
        start = index // 4 * 4
        actual = [bytes(page.names[row]).split(b"\0", 1)[0].decode() for row in range(page.count)]
        assert actual == names[start:start+4]
        assert page.file_count == len(names) and page.selected_index == index
        assert page.selected_row == index % 4
    page = Page()
    assert not lib.MediaBrowserReadPage(str(folder).encode(), b"MISSING.BMP", ctypes.byref(page))
    assert page.count == 0
    results["pagination"] = {"files": len(names), "all_selection_positions": True}
    # Empty and partial pages exercise cases not covered by the 220-file directory.
    small = folder / "small"
    small.mkdir()
    for size in (0, 1, 3, 5, 7):
        for fixture in small.iterdir():
            fixture.unlink()
        short_names = [f"IMG{item:05d}.BMP" for item in range(size)]
        for name in reversed(short_names):
            (small / name).write_bytes(b"small")
        if size == 0:
            assert not lib.MediaBrowserReadPage(str(small).encode(), b"MISSING.BMP", ctypes.byref(page))
            assert page.count == 0
        for index, name in enumerate(short_names):
            assert lib.MediaBrowserReadPage(str(small).encode(), name.encode(), ctypes.byref(page))
            first = index // 4 * 4
            actual = [bytes(page.names[row]).split(b"\0", 1)[0].decode() for row in range(page.count)]
            assert actual == short_names[first:first+4]
            assert page.selected_row == index % 4 and page.file_count == size
    results["pagination"]["small_directory_sizes"] = [0, 1, 3, 5, 7]

    # A position-dependent image reveals horizontal/vertical inversion and sampling errors.
    def bgr(x, y):
        return bytes(((x * 5 + y) % 256, (y * 7) % 256, (x * 3) % 256))

    frame = b"".join(bgr(x, y) for y in reversed(range(240)) for x in range(320))
    h = (Byte * 54)()
    lib.MediaBmpHeader(h)
    bmp = bytes(h) + frame
    bmp_path = OUT / "gradient.bmp"
    bmp_path.write_bytes(bmp)
    old_h = (Byte * 224)()
    lib.MediaAviHeader(old_h, 1, 200000)
    video_block = b"00db" + struct.pack("<I", len(frame)) + frame
    old_index = b"00db" + struct.pack("<III", 16, 4, len(frame))
    old_avi = bytes(old_h) + video_block + b"idx1" + struct.pack("<I", 16) + old_index
    old_path = OUT / "legacy.avi"
    old_path.write_bytes(old_avi)
    # Put PCM before the first video block to verify bounded skipping.
    audio = b"\x01\x00" * 4
    audio_block = b"01wb" + struct.pack("<I", len(audio)) + audio
    new_h = (Byte * 324)()
    lib.MediaAvHeader(new_h, 1, 200000, len(audio), len(audio_block)+len(video_block), 2)
    new_index = b"01wb" + struct.pack("<III", 16, 4, len(audio))
    new_index += b"00db" + struct.pack("<III", 16, 4+len(audio_block), len(frame))
    new_avi = bytes(new_h)+audio_block+video_block+b"idx1"+struct.pack("<I",32)+new_index
    new_path = OUT / "audio-first.avi"
    new_path.write_bytes(new_avi)

    def thumbnail(path, capacity=960):
        guarded = (Byte * (3456+16))(*([0x5A]*(3456+16)))
        pointer = ctypes.cast(ctypes.byref(guarded, 8), Pointer)
        work = (Byte * max(960, capacity))()
        valid = lib.MediaThumbnailRead(str(path).encode(), pointer, work, capacity)
        assert bytes(guarded[:8]) == b"\x5A"*8 and bytes(guarded[-8:]) == b"\x5A"*8
        return valid, bytes(guarded[8:-8])

    previews = []
    for path in (bmp_path, old_path, new_path):
        valid, data = thumbnail(path)
        assert valid
        for y in range(36):
            sy = ((y*2+1)*240)//72
            for x in range(48):
                sx = ((x*2+1)*320)//96
                blue, green, red = bgr(sx, sy)
                expected = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
                assert int.from_bytes(data[(y*48+x)*2:(y*48+x+1)*2], "big") == expected
        previews.append(data)
    assert previews[0] == previews[1] == previews[2]
    assert thumbnail(bmp_path, 959) == (False, bytes(3456))
    # VGA must decode every sampled pixel and reject insufficient row capacity.
    vga_header = bytearray(bytes(h))
    for offset, value in ((2, 54+640*480*3), (18, 640), (22, 480), (34, 640*480*3)):
        struct.pack_into("<I", vga_header, offset, value)
    vga_path = OUT / "vga.bmp"
    vga_path.write_bytes(vga_header + b"".join(bgr(x//2, y//2)
                                             for y in reversed(range(480)) for x in range(640)))
    assert thumbnail(vga_path, 1920) == (True, previews[0])
    assert thumbnail(vga_path, 1919) == (False, bytes(3456))
    assert thumbnail(vga_path, 960) == (False, bytes(3456))

    invalids = [bmp[:100], new_avi[:400]]
    for base, offset, value in ((bmp, 18, 319), (bmp,22,0xFFFFFF10),
                                (new_avi,328,0xFFFFFFFE), (new_avi,316,0xFFFFFFFF),
                                (old_avi,48,0), (new_avi,188,1)):
        corrupt = bytearray(base)
        struct.pack_into("<I",corrupt,offset,value)
        invalids.append(corrupt)
    for number, corrupt in enumerate(invalids):
        path = OUT / f"bad-{number}.bin"
        path.write_bytes(corrupt)
        assert thumbnail(path) == (False, bytes(3456))
    results["thumbnail"] = {"formats": ["BMP", "legacy AVI", "PCM-before-video AVI"],
                            "pixels_checked_per_format": 48*36, "rejected_fixtures": len(invalids)}

    # Render with production C code; guards catch layout writes outside the frame.
    view = View()
    for row, name in enumerate((b"IMG00001.BMP",b"IMG00002.BMP",b"VID00001.AVI",b"BROKEN.AVI")):
        view.page.names[row].value = name
    view.page.count = 4
    view.page.selected_row = 2
    view.page.selected_index = 6
    view.page.file_count = 13
    view.notice = b""
    keep_alive = [(Byte * 3456).from_buffer_copy(previews[0]) for _ in range(3)]
    for row in range(3):
        view.thumbnails[row] = keep_alive[row]
        view.valid[row] = True
    render = (Byte * (153600+16))(*([0xA5]*(153600+16)))
    target = ctypes.cast(ctypes.byref(render,8),Pointer)
    lib.MediaListRender(ctypes.byref(view),target)
    assert bytes(render[:8]) == bytes(render[-8:]) == b"\xA5"*8
    pixels = bytes(render[8:-8])
    for row in range(3):
        y = 28+row*44+4
        for dy in range(36):
            expected = previews[0][dy*96:(dy+1)*96]
            offset = ((y+dy)*320+8)*2
            assert pixels[offset:offset+96] == expected
    assert int.from_bytes(pixels[((28+2*44)*320+4)*2:((28+2*44)*320+4)*2+2],"big") == 0x07FF
    # Font clipping on every edge must preserve the frame guards.
    for x,y in ((-4,-4),(318,239),(-320,-240),(0,40)):
        lib.LcdFontDraw(target,320,240,x,y,b"TEST",2,0xFFFF)
        assert bytes(render[:8]) == bytes(render[-8:]) == b"\xA5"*8
    (OUT/"list.rgb565").write_bytes(pixels)
    subprocess.run(["ffmpeg","-v","error","-y","-f","rawvideo","-pixel_format","rgb565be",
                    "-video_size","320x240","-i",str(OUT/"list.rgb565"),"-frames:v","1",
                    str(OUT/"list.png")],check=True)
    results["render"] = {"rows":4,"selection_highlight":True,"invalid_preview_placeholder":True,
                         "bounds_guards":True,"artifact":str(OUT/"list.png")}
(OUT/"summary.json").write_text(json.dumps(results,indent=2))
print("PASS: actual C pagination (220 files/all positions), BMP/AVI thumbnails, malformed inputs, list render/guards")
