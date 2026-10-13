"""Build actual LVGL and the production menu on the host, then verify rendering.

Uses the resolved managed LVGL version and firmware LVGL config. No device or
TF files are accessed; builds and screenshots remain under ignored outputs.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "outputs/lvgl-tests"
OUT.mkdir(parents=True, exist_ok=True)
LVGL = ROOT / "managed_components/lvgl__lvgl"
SDKCONFIG = ROOT / "build/config/sdkconfig.h"
if not LVGL.exists() or not SDKCONFIG.exists():
    raise SystemExit("Run idf.py build first to resolve LVGL and firmware configuration")
# Use the same Kconfig selections on the host, without ESP_PLATFORM dependencies.
config = [line for line in SDKCONFIG.read_text().splitlines()
          if line.startswith("#define CONFIG_LV_")]
(OUT / "lv_conf.h").write_text(
    "#ifndef LV_CONF_H\n#define LV_CONF_H\n" + "\n".join(config)
    + f'\n#include "{LVGL / "src/lv_conf_kconfig.h"}"\n'
    + "#undef LV_ASSERT_HANDLER\n#define LV_ASSERT_HANDLER abort();\n"
    + "#ifndef __ASSEMBLY__\n#include <stdlib.h>\n#endif\n#endif\n")
(OUT / "esp_err.h").write_text("typedef int esp_err_t;\n")
(OUT / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.22)
project(box3_lvgl_tests C CXX)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(LV_BUILD_CONF_PATH "{OUT / 'lv_conf.h'}" CACHE PATH "" FORCE)
add_subdirectory("{LVGL}" lvgl)
add_executable(menu_test "{ROOT / 'tools/test_lvgl_menu.c'}"
               "{ROOT / 'main/ui/lvgl_menu.c'}" "{ROOT / 'main/ui/camera_ui.c'}")
target_include_directories(menu_test PRIVATE "{ROOT / 'main'}" "{OUT}")
target_compile_options(menu_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(menu_test PRIVATE lvgl m)
''')
with (OUT / "build.log").open("w") as log:
    subprocess.run(["cmake", "-S", str(OUT), "-B", str(OUT / "build")],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(["cmake", "--build", str(OUT / "build"), "-j", "4"],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
subprocess.run([str(OUT / "build/menu_test")], cwd=OUT, timeout=60, check=True)
for raw in OUT.glob("*.rgb565"):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo",
                    "-pixel_format", "rgb565be", "-video_size", "320x240", "-i",
                    str(raw), "-frames:v", "1", str(raw.with_suffix(".png"))], check=True)
print(f"PASS: actual LVGL rendering and lifecycle; screenshots in {OUT}")
