# ATK ESP32-S3 BOX3 相机

当前版本 `camera-storage v2.8-resume-black`：GC0308 → ESP32-S3 → ST7789，320×240 横屏取景。支持 BMP 拍照、带 PCM 声音的 AVI 录像、设备端照片查看及录像回放。TF 卡仅接受实际 **exFAT 或 FAT32**，不将 FAT12/FAT16 当作 FAT32。当前开发板卡为 FAT32。

## 三键操作

上电时松开全部按键，尤其不要按住GPIO0的K0，否则可能进入硬件下载模式。K1/K2通过AW9523B的XIO0/XIO1读取，启动时采样松开电平。独立任务每10ms采样、40ms去抖；K2短按确认、长按取消，K1上移、K0下移。长按内部阈值保持2秒，到时立即触发，松开不补发短按；设备界面只显示“长按取消”，不显示时长。

| 页面/模式 | K2 短按 | K1 | K0 | K2 长按取消 |
|---|---|---|---|---|
| 拍照取景 | 拍照 | 切换录像模式 | 进入功能菜单 | 保持取景 |
| 录像取景 | 开始/停止录像 | 停止录像并切换拍照模式 | 停止录像并进入功能菜单 | 保持当前状态 |
| 功能菜单 / TF 子菜单 | 进入选中功能 | 上一项 | 下一项 | 返回上一级 |
| TF 卡使用状况 | 返回 TF 子菜单 | 保持页面 | 保持页面 | 返回 TF 子菜单 |
| 媒体菜单 | 打开选中文件 | 上一个文件 | 下一个文件 | 返回功能菜单 |
| 照片查看/视频播放 | 取消查看并回到菜单 | 关闭并选择上一个文件 | 关闭并选择下一个文件 | 取消查看并回到菜单 |
| 格式化提示/警告 | 确认选中项 | 向上选择取消 | 向下选择 YES | 取消并返回来源页面 |

菜单层级为：取景 → 功能菜单 → `MEDIA VIEW`（媒体查看）、`TF CARD`（TF 卡）或第三项 `BACK TO PREVIEW`（返回上一级，回到取景）。TF 卡子菜单第一项 `CARD USAGE` 显示文件系统类型、总容量、已用和可用空间（MiB，按完整簇计算，向下取整）；第二项 `FORMAT TF CARD` 进入两次确认。默认选中每页第一项，上下循环选择，长按 K2 逐级返回。正常 FAT32/exFAT 卡也可以主动格式化。格式化确认每页默认取消，取消后返回 TF 子菜单；启动时的未知格式提示取消则返回取景。

菜单采用 **LVGL 9.3.0 自带的 `lv_menu` 多级菜单组件**，使用 Montserrat 12/14 英文字体；取景标题沿用原点阵字体。`PHOTO` 拍照，`VIDEO` 录像模式，`REC` 和右上角红方块表示正在录制，`MEDIA` 文件列表，`CANCEL` 取消。菜单以每页四行显示文件，左侧为48×36缩略图，右侧为文件名及PHOTO/VIDEO类型，蓝色高亮当前选中项，标题显示“当前位置/总文件数”。底部同一行提示按 `K0: DOWN`、`K1: UP`、`K2: OPEN`、`HOLD K2: BACK` 的顺序显示。K1/K0逐项上下移动，跨页自动滚动，到两端循环选择。照片缩略图来自原图，录像缩略图来自第一帧；同页移动复用预览缓存，跨页才读取新文件。损坏或不支持的媒体显示占位标签及NO PREVIEW，不阻止其他文件浏览。

打开照片后保持显示，视频播放结束自动回到原选中列表页；返回列表保留预览，返回功能菜单或取景后释放媒体列表图像及源缩略图缓存；LVGL 对象、通用屏幕缓冲和转换后的缩略图存储保留以供下次使用。支持本项目320×240及640×480的24位BMP，以及原单视频AVI和新版带PCM音轨AVI；其他编码、尺寸或损坏文件打开时显示错误。

本版已编译烧录至 `/dev/ttyACM0`，连续10次退出菜单的黑屏、丢弃三帧和恢复预览流程通过，内存保持稳定。本次未写入、删除或格式化TF；详细范围见[当前测试报告](docs-codex/test-report.md)。

## TF 卡格式提示

物理卡初始化成功后才检测文件系统。检测到未知文件系统或 FAT12/FAT16 时进入 `FORMAT TF CARD` 并显示 `UNSUPPORTED TF` 通知，默认选中 `CANCEL`。需要格式化时，K0 选 YES、K2 确认；下一页明确提示 `WARNING: ERASE ALL DATA`，再次默认取消，需再按 K0 选 YES、K2 确认，才开始格式化为 **exFAT**。任一页按取消或长按K2取消都不擦卡。

格式化会清空整张卡，使用16KiB簇，从扇区0创建整卡单卷并替换旧分区表；请先备份卡中内容。进行中勿断电或拔卡。串口命令不能操作格式化确认页。无卡、SPI/I/O错误不会被当作未知格式并提示擦除。本版未格式化开发板卡；格式原语已在隔离的虚拟磁盘验证。正式擦除前会先用 LVGL 显示进行中提示，并等待 LCD 刷新完成；显示失败则不擦卡。

取消后仍能取景，再执行拍照、录像或浏览可重新打开格式提示。运行中不支持安全热插拔，换卡后重启。支持格式的现有卡不会被自动转换。启动日志明确输出 `fs=exFAT` 或 `fs=FAT32`。

## 图片、录像与声音

照片保存为 `DCIM/IMG00001.BMP` 等编号文件，640×480，每张921654字节，24位BGR、底部优先。视频为 `DCIM/VID00001.AVI` 等编号文件，包含未压缩BGR24视频及 **24kHz、16位、单声道PCM** 音轨。ES7210的TDM槽0/MIC1用于录音，ES8311和板上功放用于回放。录音失败或缓冲溢出会使本次录像失败，程序不会静默降级成无声录像。

录像最高采样5FPS，实际受卡速、图像转换和显示影响，当前卡实测约1.4–1.5FPS。音频独立任务持续采样，4秒PSRAM环形缓冲吸收写卡延迟，DMA及软件缓冲溢出都检查。最长60秒或300帧自动结束。音频始终保持24kHz，视频按实际PCM时长计算平均播放周期；AVI为恒定帧率，不逐帧保存可变采样间隔。

文件完整写入、同步并关闭后才将 `.TMP` 改为 `.BMP` / `.AVI`，重启跳过已有最终文件和遗留TMP。录像中断电或测试复位可能留下不能直接播放的TMP，本版不做掉电恢复，也不自动删除历史文件。照片/录像不保存取景标题和REC标记。

回放先预载PCM到PSRAM（最多64秒），独立任务驱动扬声器；主任务按播放时钟读视频，读卡过慢会跳过落后的视频帧。默认回放音量为-12dB。此前 v2.7 已通过3.36秒测试录像播放到结束；v2.8 未重新执行完整音视频回归，当前验证范围见[当前测试报告](docs-codex/test-report.md)。

拍照键经去抖确认后，LCD 将整幅画面与白色混合 25% 作为快门反馈。高亮从实际绘制完成起至少保持 0.5 秒，TF 写入更久时保持到写入结束，不再额外追加固定高亮延时；普通画面在摄像头恢复后刷新。归还预览帧后，摄像头临时切换为 GC0308 最大彩色分辨率 640×480，丢弃最初两帧，保存第三帧原始图像到 TF 卡；保存结束后恢复 320×240 预览。白色反馈和屏幕文字不会写入照片。只有写入、同步、关闭和重命名均成功才显示 `PHOTO SAVED`，从首次成功绘制提示起保持 3 秒，然后恢复普通取景标题。失败显示错误，不显示成功提示。

新建构建目录会从 `sdkconfig.defaults` 同时启用 RGB565_BE 的 QVGA/VGA 格式。已有构建若未启用 VGA，请在 `idf.py menuconfig` 的 GC0308 配置中启用 `CONFIG_CAMERA_GC0308_DVP_RGB565_BE_640X480_16FPS`，保留 QVGA 为默认格式，再执行 `idf.py build`。

## 接线与依赖

TF SPI：SCLK15、MOSI16、MISO17、CS18，与LCD共用SPI2前三根信号；LCD CS47、DC48。启动先初始化卡并切换SPI模式，再初始化LCD。摄像头引脚保持原配置，采集格式为RGB565_BE，保存时转换BGR24。

共享I2C：SDA3/SCL2。AW9523B七位地址0x59；ES7210为0x40、ES8311为0x18，esp_codec_dev接口使用其要求的八位地址。音频MCLK21、BCLK38、WS39、DIN41、DOUT40。XIO12/13开启音频供电，XIO5控制功放，录音时关闭功放。其余电池/充电配置保持原行为。

依赖锁定 esp_video 2.5.0、esp_cam_sensor 2.6.0、esp_codec_dev 1.6.2、**lvgl/lvgl 9.3.0**。首次配置由 ESP-IDF Component Manager 下载 LVGL 到被忽略的 `managed_components`，版本和校验写入 `dependencies.lock`，上游 MIT 许可证保持原样。IDF v6.1原FatFs未开启exFAT，顶层CMake自动在 `outputs/fatfs_exfat/fatfs` 生成覆盖组件，启用exFAT及修正卷标布尔配置；所有使用者共用同一头文件，静态断言检查配置。SDK原文件和组件许可证保持不变。

## Linux 编译、下载及验证

本机SDK为 `/home/user1/.espressif/v6.1/esp-idf`，使用EIM环境入口；普通ESP-IDF安装可使用自身 `export.sh`。

```bash
source /home/user1/.espressif/tools/activate_idf_v6.1.sh
idf.py -DIDF_TARGET=esp32s3 build
python -m serial.tools.list_ports -v
idf.py -p /dev/ttyACM0 flash
python tools/test_media_format.py
python tools/test_camera_ui.py
python tools/test_camera_stream.py
python tools/test_preview_warmup.py
python tools/test_key_filter.py
python tools/test_media_browser.py
python tools/test_card_format.py
python tools/test_lvgl_menu.py
python tools/serial_verify.py --port /dev/ttyACM0 --seconds 30 --reset --log outputs/serial-av.log
python tools/verify_media_browser.py --port /dev/ttyACM0 --output outputs/board-remap
python tools/verify_lvgl_menu.py --port /dev/ttyACM0 --output outputs/lvgl-board-menu
python tools/verify_photo_capture.py --port /dev/ttyACM0 --count 3 --output outputs/photo-capture
python tools/verify_camera_av.py --port /dev/ttyACM0 --seconds 4 --output outputs/board-av
python tools/verify_storage.py --port /dev/ttyACM0 --allow-fat --record-seconds 61 --log outputs/storage-auto-stop-av.log
```

按实际端口修改 `/dev/ttyACM0`，串口工具不能同时占用设备。下载会替换现有固件。主机测试需要C编译器 `cc`；LVGL 主机渲染测试还需要 `cmake`、C++ 编译器及构建工具（本机为 Make），并在固件构建解析依赖后运行。格式测试的Windows路径保留LLVM实现。列表渲染及完整音视频验收脚本还需要 `ffmpeg` / `ffprobe`，这两个工具仅用于主机解码验证，不进入固件。`test_card_format.py` 在构建生成FatFs覆盖组件后运行，只使用内存磁盘。

当前设备为Espressif 303a:1001。若出现Permission denied，可长期使用 `sudo usermod -aG dialout user1` 后重新登录，或临时执行 `sudo chown user1:dialout /dev/ttyACM0`；设备重连可能恢复权限。本版通过 IDF v6.1 构建并烧录，写入校验通过。实板连续10次退出菜单的黑屏过渡和内存稳定性通过，黑屏持续约217～225毫秒。本次未写入、格式化或删除TF文件；自动验证结束时停留功能菜单，摄像头暂停。用户现场确认短暂黑屏后正常取景，没有花屏。

`verify_media_browser.py` 只读已有文件，验证列表翻页、缩略图缓存、照片/录像打开返回及退出内存回收，导出实际菜单RGB565图并核对CRC；测试结束让设备停留在第一列表页。

`verify_photo_capture.py` 默认新增三张照片，逐次核对 VGA 采集、QVGA 恢复、文件完整读回，以及从实际绘制起计时的 3 秒成功提示；测试完成停留在拍照取景页。串口日志记录 PHOTO_FEEDBACK、PHOTO_NOTICE shown/expired，供实板核对顺序和时间。

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
| `u` | 查询页面、模式、文件、挂载状态，以及摄像头 streaming 和累计取帧 captures |
| `v` | 最近保存文件完整读回，输出长度和CRC |
| `d` | 最近保存文件HEX导出，供主机解码；录制/回放时拒绝 |
| `t` | 在功能、TF、容量、格式确认或媒体列表页只读导出实际已显示的RGB565缓冲及CRC；不改变选择，取景/回放页拒绝 |

`display_ms` 包含文件操作和回放耗时，不是曝光到屏幕的端到端延迟；`STATS fps` 是应用循环帧率，不是视频采样帧率。无卡、卡满、掉电、长期运行和exFAT实卡仍需单独验收。

设计见[实现方案](docs-codex/DESIGN.md)，证据见[测试报告](docs-codex/test-report.md)。build、managed_components、outputs、sdkconfig及Python缓存由 `.gitignore` 排除，不加入Git。

## LVGL 菜单配置与扩展

使用 [LVGL 9.3 的原生 lv_menu](https://docs.lvgl.io/9.3/details/widgets/menu.html)，无需另装第三方菜单库。功能菜单、TF 子菜单、容量页、两次确认页、格式化进行中和媒体缩略图列表均由 LVGL 页面、标签、菜单行及图像组件绘制。取景、照片查看和视频画面继续使用现有同步 LCD DMA 接口。

新建构建默认启用 `CONFIG_LV_COLOR_DEPTH_16`、`CONFIG_LV_USE_MENU`、`CONFIG_LV_OS_NONE`、64KiB 内建内存池及 `CONFIG_LV_FONT_MONTSERRAT_12/14`，关闭默认主题和示例。已有 `sdkconfig` 若在迁移前已经生成 LVGL 配置，请通过 `idf.py menuconfig` 检查这几项，尤其启用 Montserrat 12 字体。不要启用多个软件绘制单元或另外创建 GUI 刷新任务；本版全部 LVGL 调用由相机主任务串行完成。

功能定义继续集中在 `main/ui/camera_ui.c` 的 `kMainMenu` / `kTfMenu` 表中。新增功能时：

1. 增加 `CameraUiScreen` / `CameraUiAction` 枚举和菜单表项，页面枚举放在 `UI_SCREEN_COUNT` 之前。
2. 在状态机中定义确认、取消、返回行为，在存储/业务层执行对应动作。
3. 在 `main/ui/lvgl_menu.c` 创建目标页面并同步正文或控件；普通功能行由菜单表自动创建，高亮及滚动复用现有实现。
4. 涉及擦除的业务继续经过独立确认，不能从 LVGL 点击回调直接调用格式化。

渲染采用 12 行 RGB565 软件缓冲，flush 将结果转为大端 RGB565 写入 PSRAM 全屏缓冲；随后由原 LCD 内部 DMA 双条缓冲显示并等待完成。LVGL 不在独立线程争用 TF/LCD 的共享 SPI。菜单只在状态变化时重绘，未启用后台动画。

`python tools/test_lvgl_menu.py` 使用实际 LVGL 和固件解析的配置，检查菜单对象、焦点、逐像素缩略图、输出边界、两页默认取消、毫秒计时回绕和重复切页内存稳定性，输出 `outputs/lvgl-tests/*.png`。这属于主机软件渲染验收，不能替代实体按键、面板颜色和共享 SPI 的实板验证。

`verify_lvgl_menu.py` 只读验证功能菜单、TF 子菜单、真实容量、第一页默认取消，以及串口模拟键无法操作格式确认。导出实际帧并核对 CRC，复位离开被保护的确认页，再检查菜单进出内存稳定性（默认四次，可用 `--cycles 20`）；结束停留功能菜单，不拍照、录像或擦卡。实板导出图见 `outputs/lvgl-board-menu/*.png`，已有媒体图见 `outputs/lvgl-board-browser/*.png`。

## LCD局部色线诊断

针对开机蓝色画面黑线、白色画面黄线，已用LCD SPI配置20MHz及180度方向对照：用户确认黄色横线均固定在屏幕物理左上角，判断更偏向面板侧蓝色显示通路异常。对照结束后恢复正常60MHz配置及取景。项目默认仍为60MHz，`idf.py menuconfig` → BOX3 camera storage → LCD SPI clock frequency可调整。黑/黄两种颜色均符合蓝色分量缺失，但发送缓冲正常不代表面板显示正常，排查见[诊断记录](docs-codex/lcd-line-diagnosis.md)。

无录像、无待拍照的取景页可通过串口发送大写B/W/R/G/K保持蓝/白/红/绿/黑纯色，X切换180度方向，b或实体K2长按退出并恢复正常方向。诊断不写TF，不接管格式确认。纯色保持时普通拍照、录像和菜单命令被丢弃。

```bash
python tools/verify_lcd_diagnostic.py --port /dev/ttyACM0 --reset --sweep --color white
# 在纯色诊断状态下对照旋转后的同一颜色；屏幕保持到下一条命令。
python tools/verify_lcd_diagnostic.py --port /dev/ttyACM0 --color white --rotate
```

测试逐条核对真正DMA源数据和CRC，不能读取面板像素。程序结束保留指定纯色供观察；重启退出保持模式，SPI频率仍由当前固件配置决定。

## 进入菜单时的摄像头处理（v2.7）

进入功能菜单先结束正在录制的AVI和录音，归还最后一张借用帧，再通过 `VIDIOC_STREAMOFF` 停止摄像头输出/采集DMA。功能菜单、TF子菜单、卡状态、确认页、照片查看、视频回放和LCD纯色诊断均在无相机帧时独立处理；主任务空闲时每10ms轮询输入，LVGL只在页面变化时刷新。文件扫描、媒体解码及同步LCD传输仍有自身耗时，10ms不是全部操作的端到端时延保证。

设备、格式和三张PSRAM映射缓冲保留。返回取景时重新 `QBUF` 三张缓冲再 `STREAMON`，不用重新复位传感器或分配完整图像。暂停要求全部借用帧已归还；恢复失败关闭设备并进入最多三次的既有有限重建流程。传感器没有断电，不能把暂停等同于释放全部内存或停止板上外部振荡器。

菜单日志为 `MENU_IDLE streaming=0 captures=... poll_ms=10`，累计取帧值应保持不变，`STATS fps=0.00`表示暂停采集；返回取景后恢复正常FPS统计。`captures`统计成功DQBUF次数，不是独立硬件DMA计数；实际停流依据STREAMOFF成功及驱动停止传感器/摄像头控制器。

```bash
python tools/test_camera_stream.py
python tools/verify_lvgl_menu.py --port /dev/ttyACM0 --cycles 20 --output outputs/menu-idle-board
python tools/verify_camera_av.py --port /dev/ttyACM0 --seconds 3 --output outputs/menu-idle-av
```

最后一个AV脚本会新增一张照片及一段录像，并用“录像中进入菜单”结束录制，核对菜单停流、返回恢复及完整媒体回放。菜单验证脚本只读TF。

## 退出菜单的黑屏过渡（v2.8）

退出菜单先用 `LcdFill(0x0000)` 显示整屏黑色并等待LCD DMA完成，再恢复摄像头采集。前三张过渡帧仅获取、归还，不读取或绘制像素；随后显示正常预览。这样无需保存旧帧或额外分配全屏图像，菜单期间仍保持停流。纯色诊断退出、初始格式提示取消也走此恢复路径。

本版实板连续10次过渡通过，黑屏到正常预览约0.22秒；日志顺序为 `PREVIEW_RESUME_BLACK` → `CAMERA_RESUMED` → 三次 `PREVIEW_WARMUP` → `PREVIEW_RESUME_SHOWN`。预热取帧/归还失败走有限恢复并重新预热，失败期间保持黑屏。采集首次正常启动不额外增加这个过渡。

```bash
python tools/test_preview_warmup.py
python tools/verify_lvgl_menu.py --port /dev/ttyACM0 --cycles 10 --output outputs/resume-black-board
```

前三帧即使元数据标记有效也不显示；元数据有效不能保证传感器刚启流时图像内容稳定。用户已现场确认退出菜单时短暂黑屏后正常取景，没有花屏。
