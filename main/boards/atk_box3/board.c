/**
 * @file board.c
 * @author Codex
 * @brief BOX3 的 AW9523B 寄存器访问与摄像头供电。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：修正历史 TCA95xx 方向寄存器不兼容问题。
 * @note 法律信息：None；寄存器依据 AWINIC 数据手册，未复制第三方实现。
 */
#include "board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* AW9523B 的方向寄存器是 0x04/05，0x06/07 是中断配置。 */
#define AW_ADDRESS       (0x59)
#define AW_REG_OUTPUT    (0x02)
#define AW_REG_DIRECTION (0x04)
#define AW_REG_ID        (0x10)
#define AW_CHIP_ID       (0x23)
#define AW_REG_MODE      (0x12)
#define I2C_TIMEOUT_MS   (100)
#define XIO_BACKLIGHT    (1U << 8)
#define XIO_CAMERA_POWER (1U << 14)
#define XIO_CAMERA_RESET (1U << 15)
#define XIO_MAIN_POWER   (1U << 11)
#define XIO_AUDIO_POWER  ((1U << 12) | (1U << 13))
#define XIO_KEYS         ((1U << 0) | (1U << 1))
#define XIO_AMPLIFIER    (1U << 5)

static const char *kTag = "board";
/* 设备句柄仅由启动/预览任务访问，存活至重启，不跨任务共享。 */
static i2c_master_dev_handle_t s_expander = NULL;

/**
 * @authors Codex
 * @brief 先取消两路设备选择，再创建共用 SPI；TF 必须先切换为 SPI 模式。
 * @return ESP_OK 或初始化错误，不释放总线，存活至重启。
 */
esp_err_t BoardInitSpi(void)
{
    /* 最新修改：总线所有权移至板级，LCD 和存储只创建设备。
     * CS18 是用户确认的 TF 卡片选，CS47 是现有 LCD 片选。
     * 上电先拉高，避免初始化另一设备时误选中。
     * 最大事务为 40 行 LCD 条带，SD 的 512 字节块也可容纳。
     * SD 先初始化，避免原生 SD 模式误解后续 LCD 时钟。 */
    gpio_config_t selects = {
        .pin_bit_mask = (1ULL << 18) | (1ULL << 47),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&selects), kTag, "SPI chip selects");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_18, 1), kTag, "TF deselect");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_47, 1), kTag, "LCD deselect");
    spi_bus_config_t bus_config = {
        .mosi_io_num = 16,
        .miso_io_num = 17,
        .sclk_io_num = 15,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PREVIEW_WIDTH * 40 * 2,
    };
    return spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
}

/**
 * @authors Codex
 * @brief 读取相邻两个 AW9523B 寄存器组成的 16 位端口状态。
 * @param reg 起始寄存器地址。
 * @param value 非空输出指针，由调用者持有。
 * @return ESP_OK 或传输错误；设备须先创建。
 */
static esp_err_t ReadPair(uint8_t reg, uint16_t *value)
{
    uint8_t bytes[2] = {0};
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_expander, &reg, 1, bytes,
                                                    sizeof(bytes),
                                                    I2C_TIMEOUT_MS),
                        kTag, "read pair");
    *value = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 读改写端口位并核对读回值，保留未使用引脚。
 * @param reg 起始寄存器。
 * @param mask 要修改的 XIO 位掩码。
 * @param bits 目标位电平或配置，只应用 mask 范围。
 * @return ESP_OK 或传输/读回错误；不允许并发调用。
 */
static esp_err_t UpdatePair(uint8_t reg, uint16_t mask, uint16_t bits)
{
    uint16_t previous = 0;
    uint16_t actual = 0;
    ESP_RETURN_ON_ERROR(ReadPair(reg, &previous), kTag, "read before update");
    uint16_t next = (uint16_t)((previous & (~mask)) | (bits & mask));
    uint8_t data[3] = {reg, (uint8_t)next, (uint8_t)(next >> 8)};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(s_expander, data, sizeof(data), I2C_TIMEOUT_MS),
        kTag, "write pair");
    ESP_RETURN_ON_ERROR(ReadPair(reg, &actual), kTag, "read back");
    ESP_RETURN_ON_FALSE(actual == next, ESP_ERR_INVALID_RESPONSE, kTag,
                        "register 0x%02x readback mismatch", reg);
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 实现共享 I2C 与已知供电初始化，不修改充电控制。
 * @param bus 非空输出，供摄像头复用，不转移所有权。
 * @return ESP_OK 或首个错误；失败资源保留至重启。
 */
esp_err_t BoardInit(i2c_master_bus_handle_t *bus)
{
    /* 地址使用七位形式 0x59，不传入带读写位的八位地址。
     * 内部上拉只辅助总线初始化，实物上拉电阻仍由板卡提供。
     * 引脚属于本板固定连接，不能随意重用作相机 XCLK。
     * 所有扩展器寄存器读写有 100 ms 上限，错误及时返回。
     * 验证芯片身份之前没有任何扩展器配置写入。 */
    ESP_RETURN_ON_FALSE(NULL != bus, ESP_ERR_INVALID_ARG, kTag, "null bus");
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 3,
        .scl_io_num = 2,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&config, bus), kTag, "I2C init");
    i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AW_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(*bus, &device, &s_expander),
                        kTag, "AW9523B device");

    /* ID 必须匹配才执行写入，避免未知扩展器使用错误寄存器。 */
    uint8_t reg = AW_REG_ID;
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_expander, &reg, 1, &id, 1,
                                                    I2C_TIMEOUT_MS),
                        kTag, "AW9523B ID");
    ESP_LOGI(kTag, "expander address=0x59 ID=0x%02x", id);
    ESP_RETURN_ON_FALSE(AW_CHIP_ID == id, ESP_ERR_NOT_SUPPORTED, kTag,
                        "unsupported expander; no registers written");

    /* 先更新输出锁存，再切换 GPIO 模式/方向，减少切换时电平毛刺。
     * 仅开启主供电和摄像头；关闭功放、背光，复位暂时保持低。
     * 未使用端口的方向、LED 模式和输出值保持读回的原值。 */
    uint16_t outputs = XIO_MAIN_POWER | XIO_CAMERA_POWER | XIO_CAMERA_RESET
                       | XIO_BACKLIGHT | XIO_AMPLIFIER;
    uint16_t high = XIO_MAIN_POWER | XIO_CAMERA_POWER | XIO_BACKLIGHT;
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_OUTPUT, outputs, high), kTag,
                        "power");
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_MODE, outputs, outputs), kTag,
                        "GPIO mode");
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_DIRECTION, outputs, 0), kTag,
                        "outputs");
    /* 最新修改：K1/K2只配置XIO0/1，保留其他端口方向及充电逻辑。 */
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_MODE, XIO_KEYS, XIO_KEYS), kTag,
                        "key mode");
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_DIRECTION, XIO_KEYS, XIO_KEYS), kTag,
                        "key input");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(kTag, "AW9523B power/mode/direction readback OK");
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 设置摄像头复位电平并等待供电稳定。
 * @return ESP_OK 或 I2C 错误；无动态分配。
 */
esp_err_t BoardResetCamera(void)
{
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_OUTPUT, XIO_CAMERA_RESET, 0), kTag,
                        "assert camera reset");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(
        UpdatePair(AW_REG_OUTPUT, XIO_CAMERA_RESET, XIO_CAMERA_RESET), kTag,
        "release camera reset");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 通过 XIO8 的低有效电平开关背光。
 * @param enabled true 打开，false 关闭。
 * @return ESP_OK 或 I2C 错误；板级初始化成功后调用。
 */
esp_err_t BoardSetBacklight(bool enabled)
{
    return UpdatePair(AW_REG_OUTPUT, XIO_BACKLIGHT,
                      enabled ? 0 : XIO_BACKLIGHT);
}

/**
 * @authors Codex
 * @brief 读取AW9523B输入寄存器；按键任务不进行写改写。
 * @param levels 非空电平输出；只有低两位有效。
 * @return ESP_OK或I2C错误。
 */
esp_err_t BoardReadKeys(uint16_t *levels)
{
    if (NULL == levels)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ReadPair(0x00, levels);
}

/**
 * @authors Codex
 * @brief 音频供电使用XIO12/13高有效，初始化后等待100ms稳定。
 * @return ESP_OK或I2C错误。
 */
esp_err_t BoardEnableAudio(void)
{
    ESP_RETURN_ON_ERROR(
        UpdatePair(AW_REG_OUTPUT, XIO_AUDIO_POWER, XIO_AUDIO_POWER), kTag,
        "audio power");
    ESP_RETURN_ON_ERROR(
        UpdatePair(AW_REG_MODE, XIO_AUDIO_POWER, XIO_AUDIO_POWER), kTag,
        "audio GPIO");
    ESP_RETURN_ON_ERROR(UpdatePair(AW_REG_DIRECTION, XIO_AUDIO_POWER, 0), kTag,
                        "audio direction");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 只修改XIO5，输出保持串行写改写。
 * @param enabled true为功放供电。
 * @return ESP_OK或I2C错误。
 */
esp_err_t BoardSetAmplifier(bool enabled)
{
    return UpdatePair(AW_REG_OUTPUT, XIO_AMPLIFIER,
                      enabled ? XIO_AMPLIFIER : 0);
}
