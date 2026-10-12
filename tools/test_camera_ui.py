"""Compile the actual menu state machine and check confirmation/mode sequences."""
import ctypes
from pathlib import Path
import subprocess
import itertools

root = Path(__file__).resolve().parents[1]
out = root / "outputs/ui-tests"
out.mkdir(parents=True, exist_ok=True)
# Only replace the platform error type; production state-machine source is unchanged.
(out / "esp_err.h").write_text("typedef int esp_err_t;\n")
subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                "-I", str(out), str(root / "main/ui/camera_ui.c"),
                "-o", str(out / "camera_ui.so")], check=True)
lib = ctypes.CDLL(str(out / "camera_ui.so"))


class Ui(ctypes.Structure):
    _fields_ = [("screen", ctypes.c_int), ("video", ctypes.c_bool),
                ("yes", ctypes.c_bool), ("dirty", ctypes.c_bool)]


lib.CameraUiInit.argtypes = [ctypes.POINTER(Ui), ctypes.c_bool]
lib.CameraUiHandle.argtypes = [ctypes.POINTER(Ui), ctypes.c_int]
lib.CameraUiHandle.restype = ctypes.c_int
ui = Ui()
lib.CameraUiInit(ctypes.byref(ui), False)
assert ui.screen == 0 and not ui.video
assert lib.CameraUiHandle(ctypes.byref(ui), 1) == 1  # K2 photo
assert lib.CameraUiHandle(ctypes.byref(ui), 2) == 3 and ui.video  # K1 mode/up
assert lib.CameraUiHandle(ctypes.byref(ui), 1) == 2  # K2 recorder
assert lib.CameraUiHandle(ctypes.byref(ui), 3) == 4 and ui.screen == 3  # K0 menu
assert lib.CameraUiHandle(ctypes.byref(ui), 2) == 5
assert lib.CameraUiHandle(ctypes.byref(ui), 3) == 6
assert lib.CameraUiHandle(ctypes.byref(ui), 1) == 7
ui.screen = 5
assert lib.CameraUiHandle(ctypes.byref(ui), 4) == 8 and ui.screen == 3
assert lib.CameraUiHandle(ctypes.byref(ui), 4) == 8 and ui.screen == 0

# Enumerate 78,125 sequences. A format action can only follow the second YES
# page's confirmation. Repeated K2, up/cancel, or back never erases data.
formats = 0
for sequence in itertools.product(range(5), repeat=7):
    lib.CameraUiInit(ctypes.byref(ui), True)
    stage_one = False
    for key in sequence:
        previous = (ui.screen, ui.yes)
        action = lib.CameraUiHandle(ctypes.byref(ui), key)
        if previous == (1, True) and key == 1:
            stage_one = True
            assert ui.screen == 2 and not ui.yes
        if action == 9:
            formats += 1
            assert stage_one and previous == (2, True) and key == 1
            assert sequence.count(3) >= 2 and sequence.count(1) >= 2
            assert ui.screen == 0 and not ui.yes
assert formats > 0
for key in (0, 1, 2, 4):
    lib.CameraUiInit(ctypes.byref(ui), True)
    for _ in range(20):
        assert lib.CameraUiHandle(ctypes.byref(ui), key) != 9
print("PASS: actual C UI; photo/video/browser; 78125 format confirmation sequences")
