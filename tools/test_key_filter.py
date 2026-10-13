"""Execute the production key filter with precise monotonic timestamps.

Tests K2 firing while held at 2 s, no repeat or release confirmation, debounce,
near-threshold release, menu K0 confirmation/cancellation and K1/K2 navigation.
The actual page mapping and preview controls are exercised without GPIO writes.
"""
import ctypes
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "outputs/key-filter-tests"
out.mkdir(parents=True, exist_ok=True)
(out / "esp_err.h").write_text("typedef int esp_err_t;\n")
subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                "-I", str(out), str(root / "main/ui/key_filter.c"),
                str(root / "main/ui/camera_ui.c"), "-o", str(out / "filter.so")], check=True)
lib = ctypes.CDLL(str(out / "filter.so"))


class Filter(ctypes.Structure):
    _fields_ = [("event", ctypes.c_int), ("idle", ctypes.c_int),
                ("candidate", ctypes.c_int), ("stable", ctypes.c_int),
                ("changed", ctypes.c_int64), ("started", ctypes.c_int64),
                ("pressed", ctypes.c_bool), ("long_sent", ctypes.c_bool)]


lib.CameraKeyFilterInit.argtypes = [ctypes.POINTER(Filter), ctypes.c_int, ctypes.c_int]
lib.CameraKeyFilterSample.argtypes = [ctypes.POINTER(Filter), ctypes.c_int, ctypes.c_int64]
lib.CameraKeyFilterSample.restype = ctypes.c_int


def new(button=2, idle=1):
    state = Filter()
    lib.CameraKeyFilterInit(ctypes.byref(state), idle, button)
    return state


def sample(state, level, time_us):
    return lib.CameraKeyFilterSample(ctypes.byref(state), level, time_us)


# A timestamp of zero is valid. At 1,999,999 us there is no event; at exactly
# 2,000,000 us the button is still down and immediately emits KEY_CANCEL.
s = new()
assert sample(s, 0, 0) == 0
assert sample(s, 0, 39999) == 0 and not s.pressed
assert sample(s, 0, 40000) == 0 and s.pressed
assert sample(s, 0, 1999999) == 0
assert sample(s, 0, 2000000) == 4 and s.long_sent
for time_us in range(2010000, 5000001, 10000):
    assert sample(s, 0, time_us) == 0  # Staying down never repeats.
assert sample(s, 1, 5010000) == 0
assert sample(s, 1, 5050000) == 0 and not s.pressed
assert sample(s, 1, 6000000) == 0  # Lifting after long press cannot confirm.

# The next short press resets the consumed-long flag and emits exactly once.
assert sample(s, 0, 7000000) == 0
assert sample(s, 0, 7040000) == 0 and not s.long_sent
assert sample(s, 1, 7100000) == 0
assert sample(s, 1, 7140000) == 1
assert sample(s, 1, 7150000) == 0

# Debounced release time must not turn a real 1.999999-second press into long.
s = new()
assert sample(s, 0, 100000) == 0
assert sample(s, 0, 140000) == 0
assert sample(s, 1, 2099999) == 0
assert sample(s, 1, 2100000) == 0
assert sample(s, 1, 2139999) == 1
assert sample(s, 1, 2140000) == 0

# A release first observed on the deadline still classifies immediately as long.
s = new()
assert sample(s, 0, 0) == 0
assert sample(s, 0, 40000) == 0
assert sample(s, 1, 2000000) == 4
assert sample(s, 1, 2040000) == 0

# Press/release bounce shorter than 40 ms generates no action.
s = new()
for level,time_us in ((0,100000),(1,120000),(1,160000)):
    assert sample(s, level, time_us) == 0
assert not s.pressed

# K1 retains release-only up, even if held 3 s.
for button, event, idle in ((1,2,1),):
    s = new(button, idle)
    for level,time_us in ((1-idle,0),(1-idle,40000),(1-idle,3000000),(idle,3010000)):
        assert sample(s, level, time_us) == 0
    assert sample(s, idle, 3050000) == event
    assert sample(s, idle, 3060000) == 0

# Physical K2 has idle-low polarity on this board; long cancellation still
# occurs while held and cannot confirm on release.
s = new(button=2, idle=0)
assert sample(s, 1, 0) == 0
assert sample(s, 1, 40000) == 0
assert sample(s, 1, 1999999) == 0
assert sample(s, 1, 2000000) == 4
assert sample(s, 0, 2100000) == 0
assert sample(s, 0, 2140000) == 0

# Each 10 ms trace uses the real sampling period and checks the event timestamp.
for duration_ms in (50,100,799,800,1500,1990,1999,2000,2010,3000,6000):
    s = new()
    events = []
    release_us = duration_ms*1000
    timestamps = sorted(set(range(0,release_us+100001,10000)) | {release_us})
    for timestamp in timestamps:
        event = sample(s, 0 if timestamp < release_us else 1, timestamp)
        if event:
            events.append((timestamp,event))
    assert len(events) == 1, (duration_ms,events)
    if duration_ms >= 2000:
        assert events == [(2000000,4)], (duration_ms,events)
    else:
        assert events[0][1] == 1 and events[0][0] >= release_us+40000
# Exercise the production UI reducer: holding K2 must return without later
# reopening media or confirming a format option on release.
class Ui(ctypes.Structure):
    _fields_ = [("screen",ctypes.c_int),("video",ctypes.c_bool),
                ("yes",ctypes.c_bool),("format_return",ctypes.c_int),
                ("menu_selected",ctypes.c_uint),("dirty",ctypes.c_bool)]
lib.CameraUiInit.argtypes = [ctypes.POINTER(Ui),ctypes.c_bool]
lib.CameraUiHandle.argtypes = [ctypes.POINTER(Ui),ctypes.c_int]
lib.CameraUiHandle.restype = ctypes.c_int
for screen in range(9):
    ui=Ui()
    lib.CameraUiInit(ctypes.byref(ui),False)
    ui.screen=screen
    ui.yes=True
    s=new()
    assert sample(s,0,0)==0
    assert sample(s,0,40000)==0
    event=sample(s,0,2000000)
    assert event==4
    action=lib.CameraUiHandle(ctypes.byref(ui),event)
    assert action in (0,8) and action not in (1,7,9)
    expected_screen={3:6,4:3,5:3,7:6,8:7}.get(screen,0)
    assert ui.screen==expected_screen
    for level,timestamp in ((0,3000000),(0,4000000),(1,5000000),(1,5040000)):
        event=sample(s,level,timestamp)
        assert event==0
        assert lib.CameraUiHandle(ctypes.byref(ui),event)==0
        assert ui.screen==expected_screen
# Exercise the actual physical-button mapping through the production filter,
# rather than injecting logical UI events directly.
lib.CameraUiPhysicalKey.argtypes = [ctypes.c_int, ctypes.c_int]
lib.CameraUiPhysicalKey.restype = ctypes.c_int

def physical(ui, key):
    return lib.CameraUiHandle(ctypes.byref(ui), lib.CameraUiPhysicalKey(ui.screen, key))

def tap(button, idle=1):
    key=new(button,idle)
    assert sample(key,1-idle,0)==0
    assert sample(key,1-idle,40000)==0
    assert sample(key,idle,100000)==0
    return sample(key,idle,140000)
ui=Ui()
lib.CameraUiInit(ctypes.byref(ui),False)
assert physical(ui,tap(2,0))==1  # K2 photograph
assert physical(ui,tap(1))==3 and ui.video  # K1 mode
assert physical(ui,tap(2,0))==2  # K2 record toggle
assert physical(ui,tap(0))==10 and ui.screen==6  # K0 functions
assert physical(ui,tap(0))==4 and ui.screen==3  # K0 media
assert physical(ui,tap(1))==6  # K1 down
assert physical(ui,tap(2,0))==5  # K2 up
assert physical(ui,tap(0))==7  # K0 open
ui.screen=4
assert physical(ui,tap(0))==8 and ui.screen==3
# All non-preview pages consistently map physical short and long events.
for screen in range(1,9):
    assert lib.CameraUiPhysicalKey(screen,tap(0))==1
    assert lib.CameraUiPhysicalKey(screen,tap(1))==3
    assert lib.CameraUiPhysicalKey(screen,tap(2,0))==2
    assert lib.CameraUiPhysicalKey(screen,5)==4

# Exercise real button taps at every row in both menus. Up decrements,
# down increments, and the first/last rows wrap without opening an item.
for screen,count in ((6,3),(7,2)):
    for selected in range(count):
        for button,delta,idle in ((1,1,1),(2,-1,0)):
            ui=Ui()
            lib.CameraUiInit(ctypes.byref(ui),False)
            ui.screen=screen
            ui.menu_selected=selected
            ui.dirty=False
            assert physical(ui,tap(button,idle))==0
            assert ui.screen==screen and ui.dirty
            assert ui.menu_selected==(selected+delta)%count

# Both menu confirmation levels require K1 YES followed by K0 confirm.
ui.screen=1
ui.yes=False
assert physical(ui,tap(1))==0 and ui.yes
assert physical(ui,tap(2,0))==0 and not ui.yes  # K2 up selects CANCEL
assert physical(ui,tap(1))==0 and ui.yes
assert physical(ui,tap(0))==0 and ui.screen==2 and not ui.yes
assert physical(ui,tap(1))==0 and ui.yes
assert physical(ui,tap(0))==9

# K0 cancels every menu page at the deadline; release never confirms again.
for screen in range(1,9):
    ui=Ui()
    lib.CameraUiInit(ctypes.byref(ui),False)
    ui.screen=screen
    ui.yes=True
    key=new(0,1)
    assert sample(key,0,0)==0 and sample(key,0,40000)==0
    assert physical(ui,sample(key,0,2000000)) in (0,8)
    assert ui.screen=={3:6,4:3,5:3,7:6,8:7}.get(screen,0)
    assert sample(key,0,3000000)==0
    assert sample(key,1,3100000)==0
    assert sample(key,1,3140000)==0
    # K2 long is ignored inside menus, never mistaken for cancel or confirm.
    assert lib.CameraUiPhysicalKey(screen,4)==0

# K0 long in preview does not enter/confirm a menu. K1 never cancels.
assert lib.CameraUiPhysicalKey(0,5)==0
key=new(1,1)
assert sample(key,0,0)==0 and sample(key,0,40000)==0
assert sample(key,0,3000000)==0
# A just-below-deadline K0 release is still exactly one short menu confirmation.
key=new(0,1)
assert sample(key,0,0)==0 and sample(key,0,40000)==0
assert sample(key,1,1999999)==0
assert sample(key,1,2039999)==3
# Release first observed at the deadline is cancellation, even after a scheduler gap.
key=new(0,1)
assert sample(key,0,0)==0 and sample(key,0,40000)==0
assert sample(key,1,2000000)==5
assert sample(key,1,2040000)==0
print("PASS: physical K1 down/K2 up at every function/TF menu row, including wrap")
print("PASS: actual physical K0/K1/K2 mapping; capture/mode/menu/up/down/confirm/cancel")
print("PASS: immediate K0/K2 long press through actual UI; no media reopen or format confirmation on release")
print("PASS: actual C K2 filter; 1.999999s/2s boundary; while-held trigger; no repeat/release confirm; debounce; K0/K1")
