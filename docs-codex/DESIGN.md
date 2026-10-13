# 相机三键、音视频与LVGL多级菜单实现方案

更新：2026-10-13；当前版本 `camera-storage v2.8-resume-black`。遵循docs/AGENTS_CN.md及docs/ARCHITECTURE.md；docs/REQUIREMENTS.md、docs/DESIGN.md、docs/TEST.md不存在。docs目录保持只读，设计和测试报告放在docs-codex。

## 目标与模块

正常取景K2拍照或开始/停止录像，K1切换拍照/录像模式，K0进入功能菜单。菜单中K1向上、K0向下，K2短按确认/打开、长按取消；照片/录像查看时K2短按或长按取消均关闭回到列表。未知格式卡的两层确认页中K1选择取消、K0选择YES，K2确认或长按取消。长按仍到2秒立即触发，设备只显示HOLD K2: CANCEL，不显示阈值。

| 模块 | 职责和所有权 |
|---|---|
| application.c | 初始化与主循环，独占相机帧借用/归还、串行调用存储及显示 |
| boards/atk_box3/board.* | 电源、共享I2C/SPI、AW9523B、XIO按键原始读及功放控制 |
| ui/key_filter.* | 无硬件依赖的去抖/计时过滤，2秒K2即时长按和释放抑制 |
| ui/camera_keys.* | 独立10ms采样任务，40ms去抖、2秒K2即时取消，16项事件队列 |
| ui/camera_ui.* | 无硬件副作用的页面/模式/两次确认状态机，供主机穷举测试 |
| audio/camera_audio.* | ES7210录音环形缓冲、溢出检测、统计和ES8311独立PCM回放 |
| storage/camera_storage.* | 卡生命周期、主任务文件写入、双流索引、媒体扫描及回放解析 |
| storage/card_format.* | 只认可FS_EXFAT/FS_FAT32及固定参数整卡exFAT格式原语 |
| storage/media_browser.* | 有序四行分页，只保留选中项附近八个名称，内存不随目录增长 |
| storage/media_thumbnail.* | BMP/单视频AVI/双流AVI首帧的48x36只读预览，行缓冲复用 |
| ui/media_list.h | 四行媒体列表的借用模型，原手写渲染器已移除 |
| ui/lvgl_menu.* | LVGL原生菜单页面、标签/图像、焦点及同步RGB565输出；无存储权限 |
| display/lcd_font.* | 取景及回放标题使用原5x7字体，支持边缘和条带裁剪 |
| storage/media_format.* | RGB565转换、BMP、旧单视频AVI及新音视频AVI头 |
| display/lcd_display.* | 内部DMA条缓冲、预览/已校正图像显示、取景叠字及LVGL整屏图像同步传输 |

## 硬件和任务同步

保留GC0308 RGB565_BE 320x240、三帧PSRAM MMAP及ST7789双40行内部DMA条缓冲。水平纠正在预览拷贝及保存转换时执行，文件回放不再水平翻转。LCD每次调用等待最后DMA完成，共用SPI2串行用于TF（CS18，20MHz）与LCD（CS47/DC48，60MHz），SCLK15/MOSI16/MISO17。

共享I2C SDA3/SCL2。AW9523B七位地址0x59；XIO0/1只配置GPIO输入，不改充电引脚。K0 GPIO0低有效；K1/K2启动100ms后校准松开电平，板上实测松开为1/0，上电须松开全部按键。输入任务只读原始电平和投递枚举，不操作文件或UI。单键过滤器提取到ui/key_filter.*，其初始化接口接收物理按钮编号，集中映射K0→KEY_DOWN、K1→KEY_UP、K2→KEY_CONFIRM。输入任务按GPIO0/XIO0/XIO1的实际顺序传递编号，长按逻辑只作用于K2确认键，不依赖UI页面临时重映射。40ms确认按下后按候选按下序列起点计时，K2按住达到2000000微秒即产生KEY_CANCEL一次；持续按住不重复，释放只清理状态，不补发KEY_CONFIRM。1.999999秒前已观察释放则仍是短按，释放去抖时间不计入按住时间；K0/K1保持松开时上下动作。I2C驱动序列化输入任务与主任务的传感器、codec及供电事务。

音频供电XIO12/13，功放XIO5。ES7210七位地址0x40、ES8311为0x18；codec接口使用八位地址0x80/0x30。MCLK21/BCLK38/WS39/DIN41/DOUT40。使用锁定的Espressif esp_codec_dev 1.6.2，控制路径参考[ESP-IDF ES7210 TDM示例](https://github.com/espressif/esp-idf/blob/master/examples/peripherals/i2s/i2s_codec/i2s_es7210_tdm/main/i2s_es7210_record_example.c)，不修改或复制托管驱动源码。

RX/TX在录音和回放模式间分别创建/删除，不并发使用不同I2S格式。RX为24kHz/16bit四槽Philips TDM，取槽0/MIC1作为单声道音轨；另外三槽仅统计峰值/平方和供接线诊断。采集任务优先级8，20ms读取一块，16个DMA块约320ms队列；4秒/192000字节PSRAM环形缓冲通过互斥锁共享，存储写卡期间不持锁。DMA队列或软件缓冲溢出禁止发布录像。峰值非零只证明数据变化，人声需声音刺激及试听确认。

播放先完整预载最多64秒PCM至PSRAM（约3.07MB），TX标准I2S将单声道复制到左右声道，音量-12dB，独立任务保证读视频时继续输出声音。停止后确认任务放弃PCM指针才释放；停止超时保留内存以防释放后访问。主任务按单调播放时钟取目标视频帧，读卡滞后跳过旧帧。

## 卡生命周期和格式化

使用公开SDSPI接口分离物理卡初始化和FatFs挂载。物理初始化失败显示无卡/IO错误，绝不提供格式化。ff_diskio动态取得卷编号，VFS创建FATFS对象；f_mount返回FR_NO_FILESYSTEM，或成功挂载却fs_type为FAT12/FAT16时进入格式提示。其他挂载错误撤销资源并提示IO故障。

只有实体按键可操作格式提示和确认页，串口模拟键在这两页全部拒绝。每页默认取消，第一页YES确认只进入第二页，第二页再选YES确认才生成UI_ACTION_FORMAT。正常 FAT32/exFAT 卡可通过 TF 子菜单主动格式化；FormatCard 再次检查有效物理卡、卷及无活动文件。格式化工作缓冲在内部DMA内存，分配失败不写卡。擦除前必须完成LVGL进行中消息的软件绘制及LCD DMA；显示失败释放工作缓冲并返回错误。

CardFormatExfat使用FM_EXFAT|FM_SFD、单FAT和16KiB簇，从扇区0建立整卡单卷并替换旧分区表。用户必须先备份全部数据。格式化之后重新挂载并核对FS_EXFAT，再创建DCIM；失败显示错误，不无限重试擦除。启动未知格式提示取消返回取景，再执行存储/浏览可重新进入提示；主动格式化取消返回TF子菜单。

错误撤销顺序为卸载FatFs、注销VFS、注销diskio、删除SDSPI设备、释放卡结构，最后恢复CS18高电平，不释放共享SPI2。文件和播放在撤销卡前结束。无热插拔检测，换卡需重启。

IDF v6.1固定FF_FS_EXFAT=0，继续使用CMake生成的outputs/fatfs_exfat/fatfs覆盖组件启用exFAT及修正卷标宏，所有组件保持一致FSIZE_t布局。保留FatFs、SDK和codec原许可证，不把复制组件纳入Git。

## 保存格式与失败处理

照片为640×480、54字节头加921600字节BGR24数据的BMP。转换以16行/15360字节分块进行，输入帧只借用到StorageProcess返回，application统一归还。取景文字和录像标记只写LCD条缓冲，不污染保存图像。

新AVI头324字节，包含原视频strl和新增100字节PCM strl，movi从320字节类型标识开始。视频00db，音频01wb，每块8字节头；PCM偶数字节无额外对齐填充。主任务每轮及每次完整写帧后排空音频，实际块长度累积movi大小。4096项、每项16字节的PSRAM索引记录真实块偏移，旧固定帧大小算法不再用于新文件。

停止先结束采样并排空PCM，样本总字节必须等于采集统计样本数乘2，随后写idx1并回填头。音频始终24kHz，视频平均周期=实际PCM时长/视频帧数，使两个流总时长一致；不保留逐帧可变时间戳。录像仍最多60秒/300帧，采样上限5FPS；卡慢时从写完后等200ms，不补积压帧。

IMG/VID五位编号每次查盘，使用O_EXCL创建TMP，重启跳过历史最终文件和遗留TMP。完整写入、fflush/fsync/fclose及rename全部成功才发布。采集溢出、短写、索引耗尽和停止失败丢弃本次TMP并记录；物理断电可能留下TMP，不做掉电恢复，不删除历史媒体。

## 浏览和调试

菜单每页四行，顶部28像素标题显示当前位置/文件总数；每行44像素，左侧48×36缩略图，右侧文件名及PHOTO/VIDEO类型。选中行蓝色背景和青色边线，底部同一行按K0下移、K1上移、K2打开、HOLD K2返回的顺序显示提示，不显示长按时长。选中项跨页时自动滚动，首尾仍循环选择。空目录显示提示，无法生成预览时显示占位标签与NO PREVIEW。

媒体分页根据选中项的字典序位置，保留前四/后四候选，再生成对应四行页，不缓存全目录。缩略图读取只打开源文件并解析像素偏移；AVI在有界movi范围内跳过前置PCM块，取第一个完整00db视频帧，不加载音轨或索引。按目标像素中心抽样36行、每行48像素，源行960字节复用s_pixels缓冲，转换为大端RGB565；文件已有水平校正，此处只处理底部优先行序。所有长度、尺寸、位深和RIFF范围先验证。

媒体列表完整图像153600字节、四个源缩略图13824字节，进入列表时从PSRAM分配。另有153600字节通用菜单图像首次进入菜单时分配并常驻PSRAM；LVGL使用64KiB内建池、7680字节局部绘制数组及13824字节静态主机序缩略图数组。以页内名称匹配缓存，同页移动只更新高亮，不重复读源文件；跨页重新加载对应文件，失败预览也缓存其状态。打开媒体后返回保留列表缓存及选中项，返回功能菜单、取景或卸载卡时释放媒体列表图像及源缩略图并清空指针；LVGL固定对象及复制后的缩略图仍保留。分配失败不关闭拍照/录像，只显示菜单内存错误。

列表图由LVGL 9.3 lv_menu页面及lv_image/lv_label组件生成，再以LcdDrawImage分条提交。原ui/media_list.c手写渲染器删除，仅保留模型头文件；菜单使用Montserrat 12/14字体，取景叠字继续使用原点阵字体。串口t导出的正是提交给LCD的实际RGB565缓冲及CRC，主机可以检查界面与图像方向，不能替代面板物理观感。

固定DCIM目录只接受有界名称的普通BMP/AVI文件。每次扫描计算文件总数、字典序最近项和边界项，无需缓存全目录；上下循环选择。BMP限定320x240或640x480/24位/BI_RGB，AVI限定本项目一流或双流格式，验证实际文件大小、声明长度、尺寸、PCM参数、movi边界和实际块数，拒绝畸形长度及超大内存申请。

音轨预载前限制样本数，再乘2，防止整数溢出。实际扫描movi重建最多300个视频偏移，不信任卡上索引作为内存指针。回放RGB565帧及PCM各自持有并在安全关闭后释放。照片停留等待确认/返回，视频完成自动回列表。

串口e/l/n/b对应K2确认/K1上移/K0下移/K2长按取消，p/r保留取景页直接拍照/录像，u查询状态，v完整读回CRC，d只读HEX导出最近文件，t导出实际列表图。导出每行120字节数据，适配USB TX缓冲，主机核对长度/CRC并使用FFmpeg独立解码。格式确认页不接受串口按键，测试不擦开发板卡。

## 验收

主机执行实际C格式代码及全部RGB565颜色，验证双流头和变长索引；纯UI状态机穷举78125组按键序列；实际FatFs在隔离64MiB内存磁盘检查未知/FAT16/FAT32/exFAT识别、参数错误保留数据、exFAT格式化、重新挂载及文件读回。

本版主机测试使用实际C分页、缩略图解析和LVGL绘制代码，覆盖220文件全部选中位置、空页/末页、BMP及旧单视频/前置PCM双流AVI、逐像素缩略图检查、畸形文件拒绝和输出边界保护。LVGL连续1000次跨页绘制检查内存稳定，确认两页均默认取消；检查对象删除、重复初始化和32位时钟回绕。本版已重编烧录，完成取景、功能/TF菜单、容量、串口确认阻断、已有媒体及循环内存检查；未修改真实TF文件，实板范围见当前测试报告。

## 本版拍照流程（2026-10-13）

1. 主任务收到 PHOTO 动作后清除旧提示、设置待拍照标志；LCD 对完整预览条缓冲混合 25% 白色。采集帧保持只读。高亮帧完成 LCD DMA 后记录微秒时间；保存结束仅补足 500000 微秒最低反馈时长，慢卡写入超过此期限则不额外等待。普通画面在 QVGA 恢复后刷新。
2. LCD DMA 完成并归还预览帧后调用 StorageCapturePending。CameraStop 释放采集资源，CameraInitMode 在分配缓冲之前枚举并设置原生 RGB565_BE VGA 格式。禁止持有旧 MMAP 帧切换分辨率。
3. 两帧预热后保存第三帧；无效帧不发布照片。照片 BMP 为 640×480 BGR24，共 921654 字节，逐行复用原有 15 KiB 工作缓冲；视频仍使用 QVGA。
4. PublishFile 完成写入、同步、关闭及重命名后才设置 PHOTO SAVED。所有拍照结果均停止 VGA、清除白色反馈并恢复 QVGA；恢复失败进入应用现有有限恢复流程。
5. 第一帧提示绘制成功后启动 3000000 微秒期限。预览继续刷新，期限到达清除成功提示，不阻塞三秒。
6. 照片查看和缩略图同时接受旧 QVGA 和新 VGA；校验完整尺寸组合及文件大小，VGA 按行抽样到屏幕尺寸，不申请完整 BGR24 副本。缩略图读取前验证源行缓冲容量。

默认配置同时编译两个 RGB565_BE 模式，默认启动模式继续为 QVGA。实板测试脚本照片长度更新为 VGA，串口 p 和实体 K2 使用同一流程。本版不修改 docs 下文件。

拍照 USB 验证脚本 `verify_photo_capture.py`，默认连续三次串口 K2 拍照，校验最大分辨率、恢复模式、成功提示顺序/三秒期限及完整文件读回。提示首次显示/到期采用模块日志记录微秒时长，不引入阻塞等待。固件启动版本当前为 v2.7-menu-idle。

## 功能菜单与开源方案（2026-10-13）

页面树：取景 → 功能菜单（媒体查看、TF 卡）；TF 卡 → 使用状况、格式化。媒体列表退出到功能菜单；状态页返回 TF 子菜单；子菜单返回功能菜单；功能菜单返回取景。进入功能菜单先结束录像并释放浏览缓存。

`CameraMenuItem` 以静态标签、目标页和动作描述功能，`CameraUiMenuItems` 同时供导航与显示使用。普通新增功能只需扩展表及页面/动作处理，选项数和循环选择自动依据表长度计算。保留原页面编号 0–5，新增菜单页 6、TF 页 7、状态页 8；同步所有 ctypes 状态结构和实板测试导航。

卡使用状况调用 f_getfree，64 位计算：簇字节数 = csize × 512；总容量 = (n_fatent − 2) × 簇字节数；可用 = free_clusters × 簇字节数；已用 = 总容量 − 可用。显示 MiB（1024² 字节）并向下取整，表示卷可分配数据区，不包含文件系统保留区域。未挂载/未知格式卡显示状态而不启动擦除。当前 SD 固定 512 字节扇区。

主动格式化复用原 CardFormatExfat（FM_EXFAT | FM_SFD，16KiB 簇）。两次分别选 YES 并按 K2 才产生 FORMAT，每页默认 CANCEL；format_return 记录来源，主动操作返回 TF 菜单、启动未知格式提示返回取景。物理来源检查继续拒绝串口操控两页确认，既有 FAT32/exFAT 卡也必须双确认。格式化结束重新挂载核对 FS_EXFAT；失败显示错误，无卡禁止确认入口。

### 开源菜单库调研

| 库 | 许可证及能力 | 对本项目的适配判断 |
|---|---|---|
| [LVGL lv_menu](https://docs.lvgl.io/9.3/details/widgets/menu.html) | C，多级页面及自动页面导航；[MIT](https://github.com/lvgl/lvgl/blob/master/LICENCE.txt) | 本版已采用9.3.0原生lv_menu；项目提供同步flush和三键状态适配 |
| [ArduinoMenu](https://github.com/neu-rah/ArduinoMenu) | LGPL-2.1；宏定义菜单、静态分配、ESP32、多种输入输出 | Arduino/C++ 接口，纯 C ESP-IDF 工程需要引入适配和运行环境 |
| [tcMenu](https://github.com/TcMenu/tcMenu) | Apache-2.0；菜单设计器、Arduino/mbed/ESP、远程控制 | 功能丰富，当前三键界面仍需渲染、输入和依赖适配 |

本版按用户要求迁移至LVGL自带菜单组件，不额外引入ArduinoMenu或tcMenu。继续使用纯C表驱动业务状态机，LVGL管理页面及绘制；危险操作授权仍保留项目自身状态机。上游 ESP-IDF exFAT 默认配置限制见 [Espressif 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/fatfs.html)，沿用已有项目覆盖组件。

## LVGL 迁移实现（v2.6-lvgl）

依赖在main/idf_component.yml固定lvgl/lvgl==9.3.0，dependencies.lock保存版本和校验。Component Manager下载托管源码，不修改上游源码或许可证，不把managed_components加入Git。采用官方[lv_menu](https://docs.lvgl.io/9.3/details/widgets/menu.html)、[Display设置接口](https://docs.lvgl.io/9.3/details/main-modules/display/setup.html)，不需要另外一套菜单库。

首次进入菜单时EnsureMenu创建PSRAM输出及LvglMenuInit。模块创建320×240 RGB565局部刷新display，lv_tick_set_cb读取esp_timer毫秒；所有API、lv_refr_now和文件系统操作均由相机主任务调用，不注册独立GUI任务或中断计时器。LV_OS_NONE、单软件绘制单元和无动画样式保证同步渲染。

顶部28像素为固定标签，原生lv_menu位于中间176像素，底部36像素显示通知及按键提示。普通功能行从CameraUiMenuItems自动创建，状态机menu_selected同步LV_STATE_FOCUSED。选择行先更新布局再lv_obj_scroll_to_view(LV_ANIM_OFF)，可支持未来超过四项的菜单。lv_menu_set_page(NULL)清理内部历史后切到业务状态对应页面，返回层级由原状态机独占，避免两个导航栈失步。

确认页各自拥有独立警告标签和CANCEL/YES行；GUI只同步format_yes，没有格式化点击回调。两页只能通过StorageProcess接收的实体事件操作。状态页显示存储层查询结果，GUI无FatFs头文件。格式化进行中使用单独无选择的消息页面，LCD提交成功后才调用擦除原语。

媒体列表最多四行，源图仍由存储层只读生成48×36 RGB565_BE。LVGL模块复制为主机序RGB565及静态lv_image_dsc_t，再用原生图像组件显示；复制文本/图像后不持有媒体模型指针。重建行前删除旧组件并失效图像缓存，防止描述符地址复用导致显示旧预览。空目录/损坏预览/内存错误均有LVGL标签分支。

flush从实际draw buffer stride读取局部区域，用memcpy取得主机序16位颜色并转为大端全屏输出；完成复制即flush_ready。整幅渲染完成后清空输出借用指针，StorageDraw清空传统caption、调用LcdDrawImage并等待全部DMA。相机预览、快门白色反馈、照片/视频画面仍走原通路；菜单切入时总是全屏重绘以覆盖之前的相机画面。

主机tools/test_lvgl_menu.py将固件已解析的CONFIG_LV_*配置用于实际LVGL源码构建，仅将断言停机改为abort便于捕获失败。生产菜单源码保持相同；覆盖六种页面、原生对象class、焦点、逐像素缩略图、缓存更新、前后保护区、消息页、1000次跨页内存稳定性、Deinit/重复Init及32位tick回绕。输出均在outputs/lvgl-tests。常规和独立干净构建都需通过，实板验收单独记录。

## 实板验收补充（2026-10-13）

USB设备/dev/ttyACM0（序列号30:ED:A0:AA:C1:74）烧录及哈希校验通过。为读取新菜单的实际显示结果，将t截图接口扩展到全部交互菜单，取景/回放仍拒绝；导出不修改format_yes或业务状态。verify_lvgl_menu.py检查容量页、第一确认页串口阻断及循环内存，使用USB复位离开确认页，不擦TF。serial_verify.py等待本次INITIALIZING再统计，排除USB残留的复位前STATS。

实板取景首帧1229ms、平均23.83FPS，TF为FAT32、总数据区509MiB/已用186MiB/可用323MiB。已有47个媒体分页、预览缓存、BMP打开返回、AVI回放启动/取消及进出菜单堆稳定性均通过；截图与原始日志保存在outputs/lvgl-board-*，详细范围见当前测试报告。真实格式化、人员实体按键及面板/声音主观验收未执行。

## LCD横线诊断补充（2026-10-13）

用户报告开机蓝色黑线、白色黄线。增加BOX3_LCD_SPI_HZ配置（默认60MHz），当前临时20MHz固件用于排除高速传输因素。LcdDiagnosticFill复用开机LcdFill的真实DMA通路，提交前校验纯色及CRC，并可切换180度mirror；不经过LVGL或取景叠字。StorageProcess只在无录像/待拍照的取景页接受B/W/R/G/K，保持期间拒绝普通存储动作，X反向，b或实体长按退出并恢复面板方向。verify_lcd_diagnostic.py核对六条窗口和CRC，不声称读回了面板像素。实际判断和人工观察状态见lcd-line-diagnosis.md。

LCD诊断人工反馈：20MHz纯白、180度方向反转后，黄色横线均仍固定在物理左上角且形状不变。实际发送数据/窗口/CRC正确，归因更偏向面板侧固定区域的蓝色显示通路，尚未通过模组替换区分面板、驱动和连接。已结束对照并恢复正常60MHz配置，诊断命令仍可维护使用。

## 一级菜单显式返回（2026-10-13）

在kMainMenu末尾增加BACK TO PREVIEW，目标UI_PREVIEW、动作UI_ACTION_CLOSE，短按K2返回取景并走已有资源回收；保持拍照/录像模式。LVGL自动按表创建第三行。循环选择测试同步三项数量，实板菜单循环改为选择末项返回，长按取消行为保留。

## v2.7 菜单停流与独立轮询（2026-10-13）

主循环按StorageNeedsCapture区分取景与无帧页面。取景保持CameraGet/PreferLatest/StorageProcess/StorageDraw/CameraReturn；切入菜单时StorageProcess先停止录像，主循环立即归还本帧、CameraPause，再StorageDraw(NULL)，不继续取帧。之后每10ms执行StorageProcess(NULL)、StorageDraw(NULL)，包括卡查询、确认、媒体播放时钟和纯色诊断；菜单静止时不重复刷LCD。

CameraPause要求无借用帧，成功STREAMOFF后才标记非采集。esp_video 2.5.0源码确认stop会停止传感器、stop/disable/delete摄像头控制器，并清空queued/done队列、重置缓冲元数据；V4L2设备及已分配映射仍保留。CameraResume对三张原MMAP索引重新QBUF，设取帧超时，再STREAMON；不重新BoardResetCamera或请求REQBUFS。失败保持非启流标志，上层CameraStop清理部分队列，再有限RecoverCamera。

菜单结束若恢复取景，恢复采集并重设统计窗口及last_valid，菜单停留不计作摄像头超时，也不算预览FPS。菜单STATS为0FPS并继续提供堆统计，MENU_IDLE打印streaming=0和累计DQBUF值。CameraGet在已停流时拒绝调用；CameraPause拒绝带借用帧停流。所有接口只由主任务调用。

StorageProcess允许NULL帧处理输入/媒体回放，拍照/录制只接受验证完整的真帧，存在录像却缺有效帧时终止本段；StorageDraw取景拒绝NULL，其他页面独立绘制。UI状态不再靠摄像头帧到达刷新，纯色诊断亦可停流。拍照VGA/QVGA切换仍由StorageCapturePending在归还帧后执行；若拍照挂载触发格式提示，下一轮停流处理。

验证：生产CameraPause/Resume定义配V4L2模型覆盖借用帧门槛、2000次队列循环、停流/排队/启流错误及关闭句柄。模型不模拟真实DMA或传感器初始化。实板20次菜单切换、源驱动停流成功、取帧计数静止/恢复、无帧照片及视频播放通过；一张VGA照片和一段5帧/3.36秒PCM录像的写入、完整读回、解码及回放到结束通过，录像由进入菜单关闭。当前报告只保留v2.7结果。

## v2.8 恢复取景黑屏保护（2026-10-13）

用户报告菜单退出后第一张图像花屏，采用黑屏替代过渡帧。应用在CameraResume之前完整LcdFill黑色，等待最后LCD DMA完成才启流；WarmupPreview获取并立即归还三张过渡帧，不调用StorageDraw、不读取像素，不用固定sleep猜测完成时间。三次对应现有三缓冲预热一轮；正式预览仍经过valid和PreferLatest校验。

黑屏绘制失败禁止继续恢复显示。启流失败保留黑屏并走既有有限RecoverCamera；预热失败重建后再丢弃三帧。记录PREVIEW_RESUME_BLACK及逐帧PREVIEW_WARMUP，只有正式StorageDraw的LCD DMA完成且CameraReturn成功后记录PREVIEW_RESUME_SHOWN，并清空过渡标志。若排队输入在首正常帧显示前又切入菜单，则撤销本次显示计时，菜单正常绘制。

保留原摄像头映射与菜单停流，不增加缓存旧帧的额外PSRAM，不延长正常上电首次采集流程。主机模型覆盖无效预热帧也归还、取帧失败、归还失败及不读取像素；实板10次确认黑屏→启流→丢弃3帧→正常显示的顺序与堆稳定性，用户现场确认短暂黑屏后正常取景，没有花屏，完成此次修复验证。
