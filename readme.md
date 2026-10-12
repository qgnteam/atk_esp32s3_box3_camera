# ATK ESP32-S3 BOX3 相机

当前版本 `camera-storage v2.3-keys`：GC0308 → ESP32-S3 → ST7789，320×240 横屏取景。支持 BMP 拍照、带 PCM 声音的 AVI 录像、设备端照片查看及录像回放。TF 卡仅接受实际 **exFAT 或 FAT32**，不将 FAT12/FAT16 当作 FAT32。当前开发板卡为 FAT32。

## 三键操作

上电时松开全部按键，尤其不要按住GPIO0的K0，否则可能进入硬件下载模式。K1/K2通过AW9523B的XIO0/XIO1读取，启动时采样松开电平。独立任务每10ms采样、40ms去抖；K2短按确认、长按取消，K1上移、K0下移。长按内部阈值保持2秒，到时立即触发，松开不补发短按；设备界面只显示“长按取消”，不显示时长。

| 页面/模式 | K2 短按 | K1 | K0 | K2 长按取消 |
|---|---|---|---|---|
| 拍照取景 | 拍照 | 切换录像模式 | 进入媒体菜单 | 保持取景 |
| 录像取景 | 开始/停止录像 | 停止录像并切换拍照模式 | 停止录像并进入媒体菜单 | 保持当前状态 |
| 媒体菜单 | 打开选中文件 | 上一个文件 | 下一个文件 | 返回取景 |
| 照片查看/视频播放 | 取消查看并回到菜单 | 关闭并选择上一个文件 | 关闭并选择下一个文件 | 取消查看并回到菜单 |
| 格式化提示/警告 | 确认选中项 | 向上选择取消 | 向下选择 YES | 取消并返回取景 |

屏幕使用英文点阵：`PHOTO` 拍照，`VIDEO` 录像模式，`REC` 和右上角红方块表示正在录制，`MEDIA` 文件列表，`CANCEL` 取消。菜单以每页四行显示文件，左侧为48×36缩略图，右侧为文件名及PHOTO/VIDEO类型，蓝色高亮当前选中项，标题显示“当前位置/总文件数”。底部同一行提示按 `K0: DOWN`、`K1: UP`、`K2: OPEN`、`HOLD K2: CANCEL` 的顺序显示。K1/K0逐项上下移动，跨页自动滚动，到两端循环选择。照片缩略图来自原图，录像缩略图来自第一帧；同页移动复用预览缓存，跨页才读取新文件。损坏或不支持的媒体显示类型占位图及NO PREVIEW，不阻止其他文件浏览。

打开照片后保持显示，视频播放结束自动回到原选中列表页；返回列表保留预览，退出到取景后释放菜单图像及缩略图内存。支持本项目320×240的24位BMP，以及原单视频AVI和新版带PCM音轨AVI；其他编码、尺寸或损坏文件打开时显示错误。

本次底部提示顺序检查使用卡上已有39个文件，未新增或删除TF文件。实测界面见[当前菜单截图](outputs/board-footer-single/menu.png)，验证记录见[当前测试报告](docs-codex/test-report.md)。

## TF 卡格式提示

物理卡初始化成功后才检测文件系统。检测到未知文件系统或 FAT12/FAT16 时提示 `TF FORMAT UNSUPPORTED`，默认选中 `CANCEL`。需要格式化时，K0 选 YES、K2 确认；下一页明确提示 `WARNING: ERASE ALL DATA`，再次默认取消，需再按 K0 选 YES、K2 确认，才开始格式化为 **exFAT**。任一页按取消或长按K2取消都不擦卡。

格式化会清空整张卡，使用16KiB簇，从扇区0创建整卡单卷并替换旧分区表；请先备份卡中内容。进行中勿断电或拔卡。串口命令不能操作格式化确认页。无卡、SPI/I/O错误不会被当作未知格式并提示擦除。当前测试未格式化开发板卡；格式原语已在隔离的虚拟磁盘验证。

取消后仍能取景，再执行拍照、录像或浏览可重新打开格式提示。运行中不支持安全热插拔，换卡后重启。支持格式的现有卡不会被自动转换。启动日志明确输出 `fs=exFAT` 或 `fs=FAT32`。

## 图片、录像与声音

照片保存为 `DCIM/IMG00001.BMP` 等编号文件，每张230454字节，24位BGR、底部优先。视频为 `DCIM/VID00001.AVI` 等编号文件，包含未压缩BGR24视频及 **24kHz、16位、单声道PCM** 音轨。ES7210的TDM槽0/MIC1用于录音，ES8311和板上功放用于回放。录音失败或缓冲溢出会使本次录像失败，程序不会静默降级成无声录像。

录像最高采样5FPS，实际受卡速、图像转换和显示影响，当前卡实测约1.4–1.5FPS。音频独立任务持续采样，4秒PSRAM环形缓冲吸收写卡延迟，DMA及软件缓冲溢出都检查。最长60秒或300帧自动结束。音频始终保持24kHz，视频按实际PCM时长计算平均播放周期；AVI为恒定帧率，不逐帧保存可变采样间隔。

文件完整写入、同步并关闭后才将 `.TMP` 改为 `.BMP` / `.AVI`，重启跳过已有最终文件和遗留TMP。录像中断电或测试复位可能留下不能直接播放的TMP，本版不做掉电恢复，也不自动删除历史文件。照片/录像不保存取景标题和REC标记。

回放先预载PCM到PSRAM（最多64秒），独立任务驱动扬声器；主任务按播放时钟读视频，读卡过慢会跳过落后的视频帧。默认回放音量为-12dB。物理声音和画面观感的验证结果见[当前测试报告](docs-codex/test-report.md)。

## 接线与依赖

TF SPI：SCLK15、MOSI16、MISO17、CS18，与LCD共用SPI2前三根信号；LCD CS47、DC48。启动先初始化卡并切换SPI模式，再初始化LCD。摄像头引脚保持原配置，采集格式为RGB565_BE，保存时转换BGR24。

共享I2C：SDA3/SCL2。AW9523B七位地址0x59；ES7210为0x40、ES8311为0x18，esp_codec_dev接口使用其要求的八位地址。音频MCLK21、BCLK38、WS39、DIN41、DOUT40。XIO12/13开启音频供电，XIO5控制功放，录音时关闭功放。其余电池/充电配置保持原行为。

依赖锁定 esp_video 2.5.0、esp_cam_sensor 2.6.0、esp_codec_dev 1.6.2。IDF v6.1原FatFs未开启exFAT，顶层CMake自动在 `outputs/fatfs_exfat/fatfs` 生成覆盖组件，启用exFAT及修正卷标布尔配置；所有使用者共用同一头文件，静态断言检查配置。SDK原文件和组件许可证保持不变。

## Linux 编译、下载及验证

本机SDK为 `/home/user1/.espressif/v6.1/esp-idf`，使用EIM环境入口；普通ESP-IDF安装可使用自身 `export.sh`。

```bash
source /home/user1/.espressif/tools/activate_idf_v6.1.sh
idf.py -DIDF_TARGET=esp32s3 build
python -m serial.tools.list_ports -v
idf.py -p /dev/ttyACM0 flash
python tools/test_media_format.py
python tools/test_camera_ui.py
python tools/test_key_filter.py
python tools/test_media_browser.py
python tools/test_card_format.py
python tools/serial_verify.py --port /dev/ttyACM0 --seconds 30 --reset --log outputs/serial-av.log
python tools/verify_media_browser.py --port /dev/ttyACM0 --output outputs/board-remap
python tools/verify_camera_av.py --port /dev/ttyACM0 --seconds 4 --output outputs/board-av
python tools/verify_storage.py --port /dev/ttyACM0 --allow-fat --record-seconds 61 --log outputs/storage-auto-stop-av.log
```

按实际端口修改 `/dev/ttyACM0`，串口工具不能同时占用设备。下载会替换现有固件。主机测试需要C编译器 `cc`；格式测试的Windows路径保留LLVM实现。列表渲染及完整音视频验收脚本还需要 `ffmpeg` / `ffprobe`，这两个工具仅用于主机解码验证，不进入固件。`test_card_format.py` 在构建生成FatFs覆盖组件后运行，只使用内存磁盘。

当前设备为Espressif 303a:1001。若出现Permission denied，可长期使用 `sudo usermod -aG dialout user1` 后重新登录，或临时执行 `sudo chown user1:dialout /dev/ttyACM0`；设备重连可能恢复权限。本次已编译并烧录，写入哈希验证通过。

`verify_media_browser.py` 只读已有文件，验证列表翻页、缩略图缓存、照片/录像打开返回及退出内存回收，导出实际菜单RGB565图并核对CRC；测试结束让设备停留在第一列表页。

`verify_camera_av.py` 会新增照片和录像，导出实际文件并核对CRC、音视频块/索引、PCM样本、音视频时长，再用FFmpeg解码并通过设备菜单回放。`verify_storage.py` 默认要求exFAT；当前FAT32卡须显式添加 `--allow-fat`，该结果不算exFAT实卡验收。测试不格式化或删除历史文件。

## Windows 编译及下载

```powershell
. ./tools/idf-env.ps1
Invoke-CameraIdf build
Invoke-CameraIdf -p COM4 flash
Invoke-CameraIdf -p COM4 monitor
```

本机Windows环境脚本默认SDK为 `D:\esp\v6.1\esp-idf`，工具目录 `C:\Espressif\tools`。首次创建构建目录运行 `Invoke-CameraIdf set-target esp32s3`。路径不同可传 `-IdfPath` / `-ToolsPath`。输出镜像仍为 `build/box3_camera_preview.bin`；本次Windows流程未重新验证。

## 串口调试

| 命令 | 作用 |
|---|---|
| `e` / `l` / `n` / `b` | 模拟K2确认/K1上移/K0下移/K2长按取消，使用与物理键相同的菜单状态机；格式确认页拒绝模拟键 |
| `p` / `r` | 取景页直接拍照/切换录像，保留自动测试接口 |
| `u` | 查询当前页面、模式、文件和挂载状态 |
| `v` | 最近保存文件完整读回，输出长度和CRC |
| `d` | 最近保存文件HEX导出，供主机解码；录制/回放时拒绝 |
| `t` | 只在媒体列表页导出实际菜单RGB565缓冲及CRC，供截图检查 |

`display_ms` 包含文件操作和回放耗时，不是曝光到屏幕的端到端延迟；`STATS fps` 是应用循环帧率，不是视频采样帧率。无卡、卡满、掉电、长期运行和exFAT实卡仍需单独验收。

设计见[实现方案](docs-codex/DESIGN.md)，证据见[测试报告](docs-codex/test-report.md)。build、managed_components、outputs、sdkconfig及Python缓存由 `.gitignore` 排除，不加入Git。
