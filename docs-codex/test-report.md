# 当前版本实机测试报告

日期：2026-10-12。版本：camera-preview v1.0，320×240 RGB565 大端，显示端水平镜像纠正。结论：编译、烧录、实时预览、10 分钟连续运行及 20 次 USB 复位启动通过；用户确认“左右已正常，画面流畅”。

## 环境和交付镜像

| 项目 | 实际值 |
|---|---|
| 工具链 | 本机 ESP-IDF v6.1；Xtensa esp-15.2.0_20251204；esptool 5.5.0 |
| 摄像头组件 | esp_video 2.5.0、esp_cam_sensor 2.6.0，dependencies.lock 锁定 |
| 设备连接 | COM4，USB Serial/JTAG |
| 主控 | ESP32-S3 QFN56 revision v0.2，CPU 240 MHz |
| Flash | 检测 16 MB，DIO 80 MHz |
| PSRAM | 检测 8 MB，Octal 80 MHz，启动内存自检通过 |
| IO 扩展器 | 0x59，ID 0x23；AW9523B 输出、模式与方向读回通过 |
| 摄像头 | GC0308，PID 0x9b |
| 采集格式 | RGBR（RGB565X），320×240，stride=640，153,600 B/帧 |
| 屏幕 | ST7789，SPI2 60 MHz，RGB565 大端，双 40 行 DMA 条缓冲 |
| 最终应用镜像 | build/box3_camera_preview.bin，0x47fc0 = 294,848 B |
| 应用分区 | 0x100000 B；镜像尺寸检查通过，余量约 72% |

最终应用 BIN SHA256：`4c567473389741e54d537a722a01118510a796cf78437d20e0061deb99aca6bb`。

最终 ELF SHA256：`4eee30d2e38e9f45f337aaf731974b86f98f4dda70207754eede1271954d92df`。最终串口日志启动摘要为 `4eee30d2e...`，与当前编译产物一致。

## 当前版本验证结果

| 验证项 | 结果与证据 |
|---|---|
| 编译 | 最终源码 build 成功；应用和 bootloader 尺寸检查通过；最终构建日志无 C 编译 warning/error |
| 烧录 | bootloader、partition table、应用写入完成；esptool 写入 Hash 校验通过，复位运行 |
| 上电/复位启动 | 日志约 1437 ms 完成首帧；应用入口至首帧 912 ms，满足 ≤3 秒目标 |
| 画面方向和流畅度 | 用户实物确认左右已正常，画面流畅 |
| 10 分钟连续运行 | 600 秒采集，575 个统计样本，最后累计 14,373 帧，无错误/恢复/崩溃/重启日志 |
| 显示帧率 | 平均 23.9956 FPS；最低窗口 21.66 FPS，最高 24.00 FPS；满足 ≥20 FPS 目标 |
| 显示处理/传输时间 | 平均 32.6202 ms/帧；这是软件处理及 LCD 传输，不是端到端延迟 |
| 有意丢弃旧帧 | 累计 0；当前实测场景显示链路跟上采集 |
| 数据变化 | 575 个统计样本的 CRC 均不同，采集画面数据持续变化 |
| 内部可用堆 | 始终 283,215 B，统计窗口未出现持续下降 |
| PSRAM 可用堆 | 始终 7,923,896 B，统计窗口未出现持续下降 |
| 重复 USB 复位 | 20/20 次通过，每次应用至首帧 912 ms，并出现 ≥15 FPS 统计；不等同于断电冷启动 |
| 最终镜像 60 秒确认 | 56 个样本，累计 1398 帧，平均 23.9582 FPS、32.6241 ms；首帧 912 ms，无错误，两个内存池可用堆保持不变 |
| 文档和源文件 | docs 目录未修改；源文件按 clang-format 排版，保留有效原始归属；各 C/H 文件有效注释占非空行 ≥30% |
| Git 排除 | build、managed_components、outputs、sdkconfig 与 Python 缓存均忽略；git diff --check 通过 |

10 分钟和 20 次复位在最终功能实现上执行；随后仅补充中文功能/同步注释并格式化，重新编译和烧录最终交付镜像。最终镜像另采集 60 秒完成启动与性能确认，结果由 outputs/final-run.json 保存。注释调整不改变预览算法、供电或 DMA 同步流程，错误日志源码行号随之更新。

## 实际执行命令与证据位置

```powershell
. ./tools/idf-env.ps1
Invoke-CameraIdf build
Invoke-CameraIdf -p COM4 flash
python tools/serial_verify.py --port COM4 --seconds 600 --reset --log outputs/soak-10min.log
python tools/verify_boots.py --port COM4 --cycles 20
python tools/serial_verify.py --port COM4 --seconds 60 --reset --log outputs/final-run.log
```

- outputs/device-id.log：芯片、Flash 和 PSRAM 身份。
- outputs/build.log、outputs/flash.log：最终构建与烧录记录。
- outputs/soak-10min.log/.json：连续运行原始日志和汇总。
- outputs/boot-cycles/boot-01～20.log/.json、summary.json：重复复位验证。
- outputs/final-run.log/.json：最终交付镜像短时确认。

日志属于本地中间证据，按项目要求不加入 Git。本报告记录当前功能版本，未把调试过程中未达标的临时镜像当通过项。

## 验证边界

未执行 2 小时稳定性、20 次断电冷启动、硬件断线/掉电故障注入、示波器测量 XCLK 或曝光至 LCD 的高速录像延迟测试。当前结果不能代替这些扩展验收；尤其不能把 32.62 ms 显示传输耗时当作端到端延迟。用户未反馈颜色异常，但本次未用标准色卡量化色彩精度。当前无 TE 同步，不承诺完全无扫描撕裂。
