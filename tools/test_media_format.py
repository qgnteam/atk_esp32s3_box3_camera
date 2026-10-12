"""Compile the actual portable C formatter and verify BMP/AVI bytes on Windows.

No card or board is required. All intermediate artifacts go in ignored outputs.
Install llvmlite into outputs/host-test-deps before running. Production C format
and conversion routines are compiled to LLVM IR and executed without changes.
"""
import argparse
import ctypes
import pathlib
import struct
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default=r"C:\Espressif\tools\esp-clang\esp-21.1.3_20260408\esp-clang\bin\clang.exe")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    output = root / "outputs" / "media-tests"
    output.mkdir(parents=True, exist_ok=True)
    (output / "string.h").write_text(
        "#include <stddef.h>\nvoid *memset(void *, int, size_t);\n"
        "void *memcpy(void *, const void *, size_t);\n", encoding="utf-8")
    # Espressif clang can emit host LLVM IR despite omitting the host backend.
    # llvmlite supplies native code generation; libc symbols come from Windows.
    sys.path.insert(0, str(root / "outputs/host-test-deps"))
    from llvmlite import binding as llvm
    target = output / "media_format.ll"
    subprocess.run([args.clang, "--target=x86_64-pc-windows-msvc", "-ffreestanding",
                    "-fno-builtin", "-O0", "-I", str(output), "-S", "-emit-llvm",
                    str(root / "main/storage/media_format.c"), "-o", str(target)], check=True)
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    libc = ctypes.CDLL("msvcrt")
    for symbol in ("memcpy", "memset"):
        llvm.add_symbol(symbol, ctypes.cast(getattr(libc, symbol), ctypes.c_void_p).value)
    module = llvm.parse_assembly(target.read_text(encoding="utf-8"))
    module.verify()
    machine = llvm.Target.from_default_triple().create_target_machine()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(machine.target_data)
    engine = llvm.create_mcjit_compiler(module, machine)
    engine.finalize_object()
    exports = ["MediaPut32", "MediaBmpHeader", "MediaAviHeader", "MediaConvertRow"]
    byte_pointer = ctypes.POINTER(ctypes.c_uint8)
    signatures = [[byte_pointer, ctypes.c_uint32], [byte_pointer],
                  [byte_pointer, ctypes.c_uint32, ctypes.c_uint32], [byte_pointer, byte_pointer]]
    class Functions:
        pass
    library = Functions()
    for name, signature in zip(exports, signatures):
        setattr(library, name, ctypes.CFUNCTYPE(None, *signature)(engine.get_function_address(name)))

    def u32(data, offset):
        return struct.unpack_from("<I", data, offset)[0]

    header = (ctypes.c_uint8 * 54)()
    library.MediaBmpHeader(header)
    bmp = bytes(header)
    assert bmp[:2] == b"BM" and u32(bmp, 2) == 230454
    assert u32(bmp, 10) == 54 and u32(bmp, 14) == 40
    assert struct.unpack_from("<iiHHI", bmp, 18) == (320, 240, 1, 24, 0)
    assert u32(bmp, 34) == 230400

    # Red/green/blue/white/black at opposite boundaries verify channel order,
    # byte order, full intensity expansion and horizontal reversal.
    row = (ctypes.c_uint8 * 640)()
    pixels = [0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000]
    for x, color in enumerate(pixels):
        row[2*x] = color >> 8
        row[2*x+1] = color & 255
    converted = (ctypes.c_uint8 * 960)()
    library.MediaConvertRow(row, converted)
    expected = [(0, 0, 255), (0, 255, 0), (255, 0, 0), (255, 255, 255), (0, 0, 0)]
    for x, color in enumerate(expected):
        assert tuple(converted[(319-x)*3:(320-x)*3]) == color
    # Check every possible RGB565 input, not just a few named colours.
    for first in range(0, 65536, 320):
        for x in range(320):
            pixel = min(first+x, 65535)
            row[2*x], row[2*x+1] = pixel >> 8, pixel & 255
        library.MediaConvertRow(row, converted)
        for x in range(320):
            pixel = min(first+x, 65535)
            b, g, r = pixel & 31, (pixel >> 5) & 63, (pixel >> 11) & 31
            assert tuple(converted[(319-x)*3:(320-x)*3]) == (
                (b << 3) | (b >> 2), (g << 2) | (g >> 4), (r << 3) | (r >> 2))

    for count in (0, 1, 2, 300):
        for period in (200000, 333333, 1000000):
            avi_header = (ctypes.c_uint8 * 224)()
            library.MediaAviHeader(avi_header, count, period)
            avi = bytes(avi_header)
            assert avi[:4] == b"RIFF" and avi[8:12] == b"AVI "
            assert u32(avi, 4) == 224 + count*230424
            assert avi[12:16] == b"LIST" and 12+8+u32(avi, 16) == 212
            assert avi[20:24] == b"hdrl" and avi[24:28] == b"avih"
            assert u32(avi, 32) == period and u32(avi, 48) == count
            assert u32(avi, 44) == (16 if count else 0)
            assert u32(avi, 56) == 1
            assert avi[108:116] == b"vidsDIB "
            assert u32(avi, 128) == period and u32(avi, 132) == 1000000
            assert u32(avi, 140) == count
            assert struct.unpack_from("<hhhh", avi, 156) == (0, 0, 320, 240)
            assert avi[164:168] == b"strf" and u32(avi, 168) == 40
            assert avi[172:212] == bmp[14:54]
            assert avi[212:216] == b"LIST" and avi[220:224] == b"movi"
            assert u32(avi, 216) == 4 + count*230408

    # Create a two-frame file and independently walk RIFF lists/chunks/index.
    # Matching the parser's offsets catches wrong RIFF sizes and idx1 bases.
    h = (ctypes.c_uint8 * 224)()
    library.MediaAviHeader(h, 2, 333333)
    frame = b"\x00\x00\xff" * (320*240)
    chunks = b"".join(b"00db" + struct.pack("<I", len(frame)) + frame for _ in range(2))
    index = b"".join(b"00db" + struct.pack("<III", 16, 4+i*230408, len(frame)) for i in range(2))
    video = bytes(h) + chunks + b"idx1" + struct.pack("<I", len(index)) + index
    assert u32(video, 4)+8 == len(video)
    cursor = 224
    for i in range(2):
        assert video[cursor:cursor+4] == b"00db"
        assert u32(video, cursor+4) == len(frame)
        cursor += 8 + u32(video, cursor+4)
    assert video[cursor:cursor+4] == b"idx1"
    assert cursor == 212+8+u32(video, 216)
    for i in range(2):
        entry = cursor+8+i*16
        target = 220+u32(video, entry+8)
        assert video[target:target+4] == video[entry:entry+4] == b"00db"
        assert u32(video, target+4) == u32(video, entry+12)
    (output / "sample.avi").write_bytes(video)
    (output / "sample.bmp").write_bytes(bmp+frame)
    print("PASS: actual C formatter; 65536 RGB565 colours; BMP; 12 AVI headers; RIFF/index offsets")


if __name__ == "__main__":
    main()
