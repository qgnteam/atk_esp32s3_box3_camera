# 相机拍照、录像与 TF 存储实现方案

更新：2026-10-12；当前版本 `camera-storage v1.2-exfat`。已启用 exFAT 并编译、烧录至 COM4，当前 FAT 卡上的照片/录像完整读回通过；exFAT 卡实测待换卡或用户格式化确认。只读取 docs/AGENTS_CN.md 和 docs/ARCHITECTURE.md，docs 目录不修改；docs/REQUIREMENTS.md、docs/DESIGN.md、docs/TEST.md 不存在。

## 保留的采集和显示基础

GC0308 RGB565_BE_320x240_20fps_subsample，V4L2 FourCC RGBR，stride=640，153,600 字节/帧。三帧 PSRAM MMAP，由 esp_video 拥有；CameraGet 成功后必须 CameraReturn 一次。取最新有效帧、校验长度和标志，摄像头错误有界恢复，LCD DMA 超时停止访问条缓冲。

ST7789 SPI2 60 MHz，双 40 行内部 DMA 条缓冲，每行按完整 RGB565 像素水平反转。存储使用相同水平映射，不修改采集内存。显示方向为 swap_xy=true、mirror_x=true、mirror_y=false、invert_color=true。I2C SDA3/SCL2，AW9523B 0x59、ID0x23，摄像头电源/复位 XIO14/15，主供电 XIO11、背光 XIO8。不修改电池和音频控制。

## TF 与总线生命周期

用户提供 GPIO15/16/17/18，结合已有 LCD 总线解释为 TF SCLK15/MOSI16/MISO17/CS18，需实物核对信号顺序。LCD CS47/DC48。BoardInitSpi 创建唯一 SPI2 总线，在任何通信前拉高两个片选。启动顺序：BoardInit → BoardInitSpi → StorageInit（TF 进入 SPI 模式）→ LcdInit/诊断 → CameraInit → 主循环。

TF 使用 ESP-IDF SDSPI、20 MHz、FatFs，支持 exFAT/FAT32，挂载 /sdcard，文件目录 DCIM。不自动格式化，卡缺失不阻止预览，按键操作时有限重试。SDK 卸载后将 CS 改成输入，应用恢复 CS18 输出高，避免卡误选影响 LCD。存储模块只卸载卡设备，不释放共用 SPI 总线。运行中不支持安全热拔卡。

IDF v6.1 的 ffconf.h 固定 FF_FS_EXFAT=0，顶层 CMake 从当前 SDK 自动生成 outputs/fatfs_exfat/fatfs 覆盖组件，只修改该副本中的 exFAT开关及 FF_USE_LABEL 布尔定义，保留全部原版权。EXTRA_COMPONENT_DIRS 选择该组件，使 FatFs/VFS/应用使用一致的64位 FSIZE_t 布局。CONFIG_FATFS_LFN_HEAP 默认开启，编译静态断言确认 exFAT与长文件名；不改全局 SDK，不将复制组件纳入 Git。SDK开关形态发生变化时 CMake 明确报错，要求重新审查。挂载后查询实际fs_type并记录，不把启用支持等同于卡已转换格式。

## 模块边界和所有权

| 模块 | 职责 |
|---|---|
| boards/atk_box3/board.* | 电源/I2C、唯一 SPI2 总线及启动片选 |
| camera/camera_capture.* | V4L2 队列、有效帧借用/归还及恢复 |
| display/lcd_display.* | LCD 独立设备、DMA 同步、取景红色录像标记 |
| storage/media_format.* | 与硬件无关的 BMP/AVI 头、小端字段、RGB565 → BGR24 |
| storage/camera_storage.* | TF 挂载、按键去抖、文件命名、图片/录像、错误收尾 |
| application.c | 串行调度显示和存储，恢复前结束录像 |
| tools/test_media_format.py | 实际格式 C 代码的本机 LLVM 执行验证 |

主循环顺序：CameraGet/PreferLatest → LcdDrawFrame（最后 DMA 完成）→ StorageProcess（同步读取当前帧）→ CameraReturn。存储不持有跨帧借用，不转移指针、不归还采集帧。状态和文件仅主任务使用，无额外工作任务。新增 15,360 字节 PSRAM 分块转换缓冲；16 行 BGR24 一次写入，避免整帧转换内存和逐行小写入。串行写卡会降低取景帧率。

## 照片和录像格式

BMP：54 字节文件/信息头，320×240、24 位 BGR、底部优先、每行960字节，四字节对齐；每张230454字节。当前 GC0308 不直接输出 JPEG，因此首版采用无损 BMP，无额外软件编码器。

AVI：RIFF、hdrl/avih、单个 strl/strh/strf、movi 中 00db 未压缩 BGR24 帧、最终 idx1。Header 共224字节，每帧230400字节加8字节块头及16字节索引。所有字段显式小端，避免结构体对齐。首个索引偏移相对 movi 标识为4；停止时根据完整帧数回填 RIFF/movi 大小、总帧数和实测平均帧周期。无音频，不逐帧保留可变采样间隔；单帧视频使用200ms周期。

录像上限采样5 FPS，不补积压帧，写入结束后至少等待200ms再采样；写卡和取景耗时会使实际帧率更低。60秒或300帧自动停止，最大约69.1MB。文件头用首帧到末帧的实际平均间隔，使正常播放速度接近采样时间。恢复摄像头或退出预览前结束录像。

## 操作与失败处理

BOOT/K0 GPIO0 低有效，稳定40ms去抖。先观察松开才接受操作，短按松开拍照，按住至少800ms后松开切换录像。录像中短按先结束录像再拍照。K1/K2 暂不使用。LCD 右上角12×12红色方块只覆盖显示缓冲，不写进保存内容。

USB串口 p/r/v 分别触发拍照/录像切换/最近文件读回，与按键使用同一主任务处理路径。不接受外部文件名。启动检查 usb_serial_jtag_is_driver_installed，必要时显式安装驱动并切换VFS输出；读取零等待，安装失败保留物理按键并禁用串口读取。读回在停止录像后以15KiB缓冲读取完整文件，校验头部声明长度、输出CRC，供硬件验证；不据此承诺图像或播放器兼容。

图片 IMG00001.BMP、视频 VID00001.AVI，创建前检查已有最终文件，用 O_EXCL 独占创建同名 TMP，重启跳过历史文件及遗留 TMP。不使用实时时钟，不按日期命名。所有数据完整写入、fflush/fsync/fclose成功后才发布最终扩展名。正常停止创建视频索引，部分帧写入失败直接丢弃本次视频，避免错误索引发布。任何写入、同步、关闭、重命名失败均记录、删除本次 TMP、卸载卡并恢复片选；下次操作重新挂载。断电可能遗留 TMP，本版无掉电文件恢复。

## 实施与验收

已完成存储接口、共享 SPI 启动、格式封装、按键、标记、有限录像、错误处理、exFAT覆盖组件、串口测试和完整文件读回、编译及烧录。当前卡实际为FAT：两照片/6秒录像通过，60秒自动停止93帧；具体最终结果见测试报告。检查 .gitignore，不纳入 build、outputs、测试下载依赖或 sdkconfig。测试报告仅记录当前版本。

当前FAT卡已确认SPI读写、复位后编号递增、串口短录像切换、60秒自动停止、完整文件读回和停止后的30秒24FPS预览。仍待exFAT卡实测、物理按键/显示标记、无卡与卡满故障注入、电脑图片颜色方向及AVI播放、未选中MISO高阻和长时间稳定性。以前的预览性能不能作为本版TF验证结果。
