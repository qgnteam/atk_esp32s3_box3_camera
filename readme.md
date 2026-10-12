# ATK ESP32-S3 BOX3 摄像头实时预览

已实现 GC0308 → ESP32-S3 → ST7789 本地实时预览，320×240 横屏、RGB565 大端，无需联网。上电短暂显示红绿蓝诊断图后自动预览。当前版本已烧录至 COM4，用户确认左右方向正常、画面流畅。性能和验证范围见 [当前测试报告](docs-codex/test-report.md)，实现细节见 [设计文档](docs-codex/DESIGN.md)。

首版通过显示条缓冲内水平反向拷贝纠正左右镜像，不包含按键、音频、联网或录像。

## 编译和烧录

已验证本机安装：ESP-IDF v6.1，`D:\esp\v6.1\esp-idf`；工具目录 `C:\Espressif\tools`。组件管理器按 main/idf_component.yml 和 dependencies.lock 下载依赖：esp_video 2.5.0、esp_cam_sensor 2.6.0 等。换工具链需重新验证。

在项目根目录的 PowerShell 执行：

```powershell
. ./tools/idf-env.ps1
Invoke-CameraIdf build
Invoke-CameraIdf -p COM4 flash
Invoke-CameraIdf -p COM4 monitor
```

新目录首次编译先运行 `Invoke-CameraIdf set-target esp32s3`。sdkconfig.defaults 提供 16 MB Flash、DIO 80 MHz、Octal PSRAM 80 MHz、CPU 240 MHz、USB Serial/JTAG 控制台和 GC0308 QVGA 原生 RGB565 配置。

安装位置不同可传 `-IdfPath` 和 `-ToolsPath`；脚本内 Python 虚拟环境及工具版本布局对应本机，其他安装需按实际路径调整。也可从正常导出环境的 ESP-IDF 终端运行标准 `idf.py build`、`idf.py -p COM4 flash monitor`。

输出应用镜像 build/box3_camera_preview.bin；标准 flash 命令还写入 bootloader 和 partition table。串口不同替换 COM4，监视器退出按 `Ctrl+]`；烧录和验证前关闭其他占用串口的工具。固件烧录会替换板上原应用。

## 使用和日志验证

连接板载摄像头并 USB 供电，上电自动进入预览。启动日志包括 AW9523B ID=0x23、GC0308 PID=0x9b、RGBR 320×240 stride=640 和 FIRST_FRAME。STATS 每秒输出显示 FPS、display_ms、累计帧数、丢帧数、CRC、内部 RAM 和 PSRAM 可用容量。

display_ms 只表示软件帧处理与屏幕传输，不是曝光到屏幕的端到端延迟。CRC 变化证明采集数据变化，画面颜色与方向需实物观察。

```powershell
. ./tools/idf-env.ps1
python tools/serial_verify.py --port COM4 --seconds 30 --reset --log outputs/serial.log
python tools/serial_verify.py --port COM4 --seconds 600 --reset --log outputs/soak-10min.log
python tools/verify_boots.py --port COM4 --cycles 20
```

脚本保存完整日志及同名 JSON，无 FPS 或出现恢复/错误/崩溃返回非零。`--reset` 通过 USB Serial/JTAG 重启设备，省略可续采当前运行；不能同时打开多个串口工具。

AW9523B 按真实方向寄存器驱动，不再使用不兼容的 TCA95xx 驱动。板上摄像头外部 XCLK 未通过示波器测量。摄像头启动失败时 LCD 保持红色诊断图，详细原因查串口。

build、managed_components、outputs、sdkconfig、Python 缓存由 .gitignore 忽略。源码、默认配置、依赖锁和当前文档可纳入版本管理。
