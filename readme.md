# ATK ESP32-S3 BOX3 相机

当前版本 `camera-storage v1.2-exfat`：GC0308 → ESP32-S3 → ST7789，320×240 横屏取景，BMP 拍照及无声、无压缩 AVI 录像，保存到 TF 卡。已启用 exFAT 支持并编译、烧录至 COM4；实物当前卡识别为 FAT，已完成照片与录像写入/读回验证，exFAT 卡实测仍待完成。详情见 [测试报告](docs-codex/test-report.md) 和 [实现设计](docs-codex/DESIGN.md)。

## 接线与 TF 卡

用户提供 TF 引脚为 GPIO15、16、17、18，按板上现有 SPI 配置解释为 SCLK=15、MOSI=16、MISO=17、CS=18。LCD 共用 SPI2 前三根信号，LCD CS=47、DC=48；摄像头引脚保持不变。请核对卡槽信号对应关系。TF 使用 SPI 模式、最高 20 MHz；启动先初始化 TF，再初始化 LCD。卡与 LCD 必须各自正确响应片选，卡的 MISO 在未选中时应高阻。

支持已有 exFAT 或 FAT32 文件系统的 TF 卡。程序绝不自动格式化卡；需要 exFAT 时请先在电脑备份内容并格式化。启动日志输出实际 `fs=exFAT` 或 `fs=FAT`，启用 exFAT 不会自动转换已有 FAT 卡。不要在写入或录像期间拔卡；换卡后建议重启。未插卡或挂载失败仍可取景，下次操作会重试挂载。

## 操作

使用直连 GPIO0 的 **BOOT/K0** 按键，K1/K2 当前未分配操作。上电或复位时不要按住 BOOT，以免进入下载模式。

| 操作 | 行为 |
|---|---|
| 短按 BOOT 后松开 | 拍照，保存 `DCIM/IMG00001.BMP` 等递增文件 |
| 按住 BOOT 至少 0.8 秒后松开 | 开始录像；再次长按松开停止录像 |
| 录像时短按松开 | 先结束并保存当前录像，再拍照 |
| 录像达到 60 秒或 300 帧 | 自动停止并保存；需要再次操作开始下一段 |

录像时 LCD 右上角出现红色方块；该标记仅用于取景，不写入照片或视频。文件写完、同步并关闭后才将 `.TMP` 改为 `.BMP` 或 `.AVI`，编号检查磁盘，重启不会覆盖已有文件。串口 `SAVED` 表示保存成功；`RECORD_START` / `RECORD_STOP` 表示录像切换；写卡失败打印错误并尝试卸载。

照片为 24 位 BMP，每张 230,454 字节。GC0308 当前输出 RGB565，本版不引入 JPEG 软件编码。录像为 AVI 容器中的 BGR24 单视频流，没有声音，录像采样上限 5 FPS；实际采样受卡速、取景、转换耗时影响，通常低于上限。按实际平均帧周期写入播放时间，间隔变化不会逐帧精确保留。最高约 1.15 MB/s，60 秒最多约 69.1 MB（十进制）。录像及拍照会降低取景帧率。

正常停止后视频才具有完整索引和文件头；录像中断电可能留下无法直接播放的 `.TMP` 文件，本版不提供掉电恢复。删除卡上的历史文件请使用电脑。

## 编译和烧录

本机工具链：ESP-IDF v6.1，`D:\esp\v6.1\esp-idf`，工具目录 `C:\Espressif\tools`。组件锁定 esp_video 2.5.0、esp_cam_sensor 2.6.0。IDF v6.1 将 `FF_FS_EXFAT` 固定为0；顶层 CMake 在 `outputs/fatfs_exfat/fatfs` 自动生成本项目 FatFs 覆盖组件，启用 exFAT，并修正关闭卷标时的布尔宏。SDK 原文件及其版权不修改。所有 FatFs 使用者共用覆盖头文件，C 静态断言检查 exFAT 和长文件名支持；不新增固件下载依赖。

```powershell
. ./tools/idf-env.ps1
Invoke-CameraIdf build
Invoke-CameraIdf -p COM4 flash
Invoke-CameraIdf -p COM4 monitor
```

首次创建构建目录运行 `Invoke-CameraIdf set-target esp32s3`。`sdkconfig.defaults` 启用 16 MB Flash、Octal PSRAM、240 MHz CPU、GC0308 RGB565 和 TF 存储。`Invoke-CameraIdf menuconfig` 的 `BOX3 camera storage` 可关闭 TF 功能。安装目录不同可向环境脚本传 `-IdfPath` 和 `-ToolsPath`；也可使用正常 ESP-IDF 终端中的 `idf.py`。输出镜像仍为 `build/box3_camera_preview.bin`，烧录会替换现有固件。COM4 按实际端口修改，串口工具不能同时占用端口。

## 验证

主机格式测试运行实际 C 代码的 LLVM IR，验证全部 RGB565 颜色、BMP 头及 AVI 长度/索引。`llvmlite` 仅用于主机测试，下载到忽略的 outputs，不进入固件依赖。

```powershell
. ./tools/idf-env.ps1
python -m pip install --target outputs/host-test-deps llvmlite
python tools/test_media_format.py
python tools/serial_verify.py --port COM4 --seconds 30 --reset --log outputs/serial-storage.log
python tools/verify_storage.py --port COM4 --record-seconds 6 --log outputs/storage-exfat.log
python tools/verify_storage.py --port COM4 --record-seconds 61 --log outputs/storage-auto-stop.log
```

`serial_verify.py` 检查预览；`verify_storage.py` 创建两个编号照片和一段录像，重新打开文件完整读取并核对文件头长度及CRC。默认要求实际挂载类型为 exFAT，若只测试 FAT 功能需显式加 `--allow-fat`；这不算 exFAT 验收。串口控制为 `p` 拍照、`r` 切换录像、`v` 读回最近保存文件。读回仅在录像停止后执行，会暂时暂停取景处理。USB 输入驱动显式安装，零等待读命令；不接电脑仍可通过 BOOT 操作。

当前 FAT 卡短录像实测平均周期664769微秒，约1.50FPS；60秒录像93帧，约1.54FPS，录像期间取景约8FPS。卡速和文件系统会影响结果，不能把5FPS上限当作保证。电脑查看图片/播放AVI、物理按键、无卡、卡满、掉电等仍待进一步实测。`display_ms` 包含本帧保存或读回耗时，不是曝光到屏幕的端到端延迟；`STATS fps` 是完整循环帧率，不是录像采样帧率。

build、managed_components、outputs、sdkconfig、Python 缓存均由 .gitignore 排除。
