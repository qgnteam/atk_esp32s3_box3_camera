# ATK_DNESP32S3_BOX3 外设与 GPIO 迁移参考

本表从本工作区板级源码提取，以 `xiaozhi-esp32_new/main/boards/alientek/atk-dnesp32s3-box3/` 为主。旧工程 `xiaozhi-esp32/main/boards/atk-dnesp32s3-box3/` 的引脚配置一致（忽略换行格式）。这是固件已定义和使用的连接，不等同于完整原理图；未使用的引脚不能直接判定为空闲。

## 1. 外设总览

| 外设 | 型号/接口 | 当前配置 |
|---|---|---|
| 主控 | ESP32-S3 | Wi-Fi 板型；当前 sdkconfig 为 16MB Flash、Octal PSRAM 80MHz；已有运行日志识别到 8MB PSRAM |
| LCD | ST7789 / SPI2 | 320×240，RGB565，SPI mode 0，60MHz |
| 音频输出 | ES8311 / I2C + I2S | DAC，24kHz，16 位；外部功放由扩展 IO 控制 |
| 音频输入 | ES7210 / I2C + I2S TDM | 24kHz，16 位，4 个 TDM 槽；应用取槽 0 + 槽 1（启用回声参考） |
| 摄像头 | GC0308 / SCCB + 8 位 DVP | 构建配置为 YUV422、320×240、20FPS |
| IO 扩展 | 源码标为 AW9523B | I2C 0x59；实际调用 `esp_io_expander_new_i2c_tca95xx_16bit`，迁移前应核对实物型号和驱动兼容性 |
| 按键 | BOOT、K1、K2 | BOOT 直连 GPIO0；K1/K2 接扩展 IO |
| LED | 红、蓝 | 接扩展 IO；无直连的 BUILTIN_LED_GPIO |
| 电源/电池 | 扩展 IO + ADC | 定义了供电、充电控制与检测；当前构造函数未启用电池管理 |

## 2. ESP32-S3 直接 GPIO

方向以 ESP32 为参照。

| GPIO | 信号 | 外设 | 方向/说明 |
|---:|---|---|---|
| 0 | BOOT_BUTTON_GPIO | BOOT 按键 | 输入，代码配置 active_level=0；迁移时保留启动下载功能 |
| 2 | I2C SCL | 共用控制总线 | I2C_NUM_0 |
| 3 | I2C SDA | 共用控制总线 | 音频、扩展器、摄像头 SCCB 共用 |
| 4 | CAM D6 | GC0308 | 输入 |
| 5 | CAM D7 | GC0308 | 输入 |
| 6 | CAM VSYNC | GC0308 | 输入 |
| 7 | CAM D0 | GC0308 | 输入 |
| 8 | CAM D1 | GC0308 | 输入 |
| 9 | CAM D2 | GC0308 | 输入 |
| 10 | CAM D3 | GC0308 | 输入 |
| 11 | CAM D4 | GC0308 | 输入 |
| 12 | CAM D5 | GC0308 | 输入 |
| 15 | LCD SCLK | SPI LCD | 输出 |
| 16 | LCD MOSI | SPI LCD | 输出 |
| 17 | LCD MISO | SPI LCD 总线 | 输入，SPI 初始化配置了此脚；不能仅因显示写操作就视为空闲 |
| 21 | AUDIO MCLK | ES8311/ES7210 | 输出，配置为采样率 ×256 |
| 38 | AUDIO BCLK | 音频总线 | 输出 |
| 39 | AUDIO WS/LRCK | 音频总线 | 输出 |
| 40 | AUDIO DOUT | ES8311 | ESP32 输出 → DAC 输入 |
| 41 | AUDIO DIN | ES7210 | ADC 输出 → ESP32 输入 |
| 42 | AW9523B INT | IO 扩展器 | 仅定义，当前板级代码未配置中断或使用此脚 |
| 45 | CAM PCLK | GC0308 | 输入 |
| 46 | CAM LREF/HREF/DE | GC0308 | 输入；宏名 CAM_PIN_LREF，传给 DVP 的 de_io |
| 47 | LCD CS | ST7789 | 输出 |
| 48 | LCD DC | ST7789 | 输出 |

电池管理使用 `ADC_UNIT_1 / ADC_CHANNEL_0`（ESP32-S3 对应 GPIO1），但当前未调用 InitializePowerManager，故属于预留的电池采样资源，需结合原理图确认连接。

以下参数为 `GPIO_NUM_NC`，不能当成 GPIO 编号使用：音频 PA、直连 LED、直连 LCD 背光、摄像头 PWDN、RESET、XCLK、SIOD、SIOC。摄像头控制实际使用共享 I2C，复位实际使用扩展器 XIO15；XCLK 未由 ESP32 输出，源码不足以确定板上时钟来源，迁移时核对原理图。

## 3. IO 扩展器全部 16 位

XIO 编号是扩展器端口索引，不是 ESP32 GPIO；`IO_EXPANDER_PIN_NUM_n` 是 `1U << n` 的位掩码。`CAM_2V8_EN=14`、`CAM_RST=15` 是索引，不能直接传给要求位掩码的 API。

| XIO | 宏名 | 用途 | 当前初始化/使用 |
|---:|---|---|---|
| 0 | XIO_KEY_K1 | K1 按键 | 输入；按钮回调读取后取反 |
| 1 | XIO_KEY_K2 | K2 按键 | 输入；按钮回调直接返回读取值 |
| 2 | XIO_BAT_CHRG_EN | 充电/电源控制 | 初始输出；电池管理中切换为输出低电平或输入，不能简单按普通使能脚处理 |
| 3 | XIO_BAT_CHRG | 充电状态 | 初始被输出掩码包含，与后续作为输入读取的意图冲突，需修正后验证 |
| 4 | XIO_ESP_ADC_SEL | ADC 路径选择 | 输出，置 1 |
| 5 | XIO_PA_CTRL | 功放控制 | 输出，置 1 开启 |
| 6 | XIO_EXT_GPIO0 | 扩展端口 0 | 初始输出，无显式电平设置；排针位置需核对原理图 |
| 7 | XIO_EXT_GPIO1 | 扩展端口 1 | 初始输出，无显式电平设置；排针位置需核对原理图 |
| 8 | XIO_LCD_BL | LCD 背光 | 输出，置 0，源码注释表示打开背光；按低有效连接核对 |
| 9 | XIO_LED_RED | 红 LED | 初始输出，未实现 LED 控制，极性未确认 |
| 10 | XIO_LED_BLUE | 蓝 LED | 初始输出，未实现 LED 控制，极性未确认 |
| 11 | XIO_VDD_3V3_EN | 3.3V 供电控制 | 输出，置 1；关机代码置 0 |
| 12 | XIO_VBAT_EN | VBAT 路径/音频供电 | 输出，置 1；具体电路含义需原理图确认 |
| 13 | XIO_VDDA_3V3_EN | 音频模拟 3.3V | 输出，置 1 |
| 14 | XIO_VDD_2V8_EN | 摄像头 2.8V | 输出，置 1 |
| 15 | XIO_TP_CAM_RESET | TP/CAM 复位 | 摄像头初始化时 0 保持 50ms，再 1 等待 50ms；未见触摸控制器初始化 |

当前 `DRV_IO_EXP_INPUT_MASK=0x0003` 只读 XIO0/1，`DRV_IO_EXP_OUTPUT_MASK=0xFFFC` 把 XIO2～15 设置为输出。`IoExpanderGetLevel()` 又把请求掩码与 0x0003 相与，因此读取 XIO3 会始终返回 0。电池检测不能照搬这段实现。

## 4. 总线地址与驱动参数

| 设备 | 原生 I2C 7 位地址 | 源码传入值 | 说明 |
|---|---|---|---|
| ES8311 | 0x18 | ES8311_CODEC_DEFAULT_ADDR = 0x30 | esp_codec_dev 内部右移 1 位 |
| ES7210 | 0x40 | ES7210_CODEC_DEFAULT_ADDR = 0x80 | esp_codec_dev 内部右移 1 位 |
| IO 扩展器 | 0x59 | AW9523B_ADDR = 0x59 | 直接传给扩展器驱动 |
| GC0308 | 0x21 | GC0308_SCCB_ADDR = 0x21 | 摄像头组件中已是 7 位地址 |

使用 ESP-IDF `i2c_device_config_t.device_address` 时填 7 位地址；沿用 `esp_codec_dev` 当前接口时传入它要求的 8 位地址。不要混用两种格式。

- I2C：SDA=3、SCL=2，启用内部上拉，glitch_ignore_cnt=7；摄像头 SCCB 配置 100kHz，并复用现有 bus_handle。
- LCD：SPI2_HOST、DMA 自动选择，队列深度 7，命令/参数均 8 位，16 位 RGB、数据大端；swap_xy=true、mirror_x=true、mirror_y=false、偏移 0/0，色彩反转 true。宏 DISPLAY_BACKLIGHT_OUTPUT_INVERT 在这里还被用于色彩反转，应区分面板色彩与背光电平。
- 音频：I2S_NUM_0，ESP32 为 master；发送为标准 I2S，接收为 TDM 槽 0～3，MCLK=256fs（24kHz 时为 6.144MHz）。应用启用 input_reference=true；这不代表板上装有 4 个麦克风，实体麦克风及参考信号连接需原理图确认。
- 摄像头：DVP 数据顺序必须保留为 `[7,8,9,10,11,12,4,5]`，VSYNC=6、DE=46、PCLK=45。配置 xclk_freq=24MHz，但 xclk_io=NC，不要直接在其他板上复制这个 NC 配置。

## 5. 移植顺序与现有代码限制

建议顺序：创建共用 I2C → 初始化并核对 IO 扩展器 → 打开主供电、音频电源及摄像头电源 → 初始化 SPI/LCD → 初始化音频 → 对摄像头执行复位时序并初始化 SCCB/DVP → 按需接入按键和电池管理。当前构造函数先创建 I2C/SPI，再初始化扩展器；音频在首次 GetAudioCodec 时创建。

可直接复用引脚和外设参数，但移植电源/按钮逻辑前需要处理：

1. 核对 AW9523B 命名与 TCA95xx 驱动的对应关系，确保方向、电平、寄存器兼容。
2. 修正 XIO3 的输入方向和读取掩码；确认充电状态低有效逻辑与电路一致。
3. InitializePowerManager、InitializeBoardPowerManager 以及 GetBatteryLevel 已被注释；K2 长按和省电关机回调仍存在访问 power_manager_ 的路径。需先初始化指针并保护这些路径，再启用关机。
4. K1 与 K2 的取值极性不同，保留源码行为供检查，但应以实测按下/释放电平决定新项目配置。
5. 没有实现扩展器背光亮度驱动；当前只在初始化写 XIO8=0，不能直接获得 PWM 调光。红/蓝 LED 和触摸也未实现完整驱动。
6. 未在此源码中配置的 USB、串口、存储卡、其他排针或 Flash/PSRAM 引脚，不据此标为可用。复用任何 GPIO 前核对实物原理图和模块内存占用。

## 6. 提取依据与验证范围

- 板级 `config.h`：引脚、XIO 映射、尺寸、采样率。
- 板级 `atk_dnesp32s3_box3.cc`：供电初值、初始化次序、ST7789、SCCB/DVP、按钮回调和未启用的电源管理。
- 板级 `power_manager.h`：ADC 单元/通道、充电检测及控制。
- 板级 `config.json`：ESP32-S3、设备 AEC、GC0308 格式配置。
- `main/audio/codecs/box_audio_codec.cc`：音频收发格式和输入槽选择。
- `managed_components/espressif__esp_codec_dev/device/include/` 及 `platform/audio_codec_ctrl_i2c.c`：音频地址值及右移转换。
- `managed_components/espressif__esp_cam_sensor/sensors/gc0308/include/gc0308.h`：摄像头地址。
- 当前 `sdkconfig` 与已有 `hardware-debug/after-flash.log`：内存配置和历史运行证据。

本次完成源码交叉核对；未重新编译、刷机或进行实物电气测试。既有日志只能证明此前运行情况，不能代替新项目的硬件验证。
