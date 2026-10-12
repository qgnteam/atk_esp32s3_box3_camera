/**
 * @file lcd_display.c
 * @author Codex
 * @brief 内部 RAM 条缓冲隔离摄像头 PSRAM 和 SPI DMA。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：双条缓冲、完成信号和超时保护；法律信息：None。
 */
#include "lcd_display.h"

#include <string.h>
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "boards/atk_box3/board.h"

/* 实测 16 行条带受任务唤醒开销限制；改 40 行，双缓冲合计 50 KiB。 */
#define STRIP_ROWS     (40)
#define STRIP_BYTES    (PREVIEW_WIDTH * STRIP_ROWS * 2)
#define LCD_TIMEOUT_MS (1000)
#define LCD_SPI_HZ     (60000000)

static const char *kTag = "lcd";
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_done = NULL;
/* 任务私有状态：最多一个 DMA 事务在途，超时保持 pending 防止覆盖。 */
static uint8_t *s_strips[2] = {NULL, NULL};
static bool s_pending = false;
static unsigned s_next_strip = 0;

/**
 * @authors Codex
 * @brief 在 SPI DMA 完成中断里仅释放同步信号。
 * @param io 驱动句柄，未使用。
 * @param event 驱动事件，未使用。
 * @param context 有效信号量，初始化后保持存活。
 * @return 是否需要唤醒高优先级任务；不阻塞、不分配内存。
 */
static bool TransferDone(esp_lcd_panel_io_handle_t io,
                         esp_lcd_panel_io_event_data_t *event, void *context)
{
    (void)io;
    (void)event;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &woken);
    /* 当前 SPI panel 驱动不消费回调返回值，显式请求 ISR 后调度。 */
    if (pdTRUE == woken)
    {
        portYIELD_FROM_ISR();
    }
    return pdTRUE == woken;
}

/**
 * @authors Codex
 * @brief 等待此前已成功提交的条缓冲完成。
 * @return ESP_OK 或 ESP_ERR_TIMEOUT；超时后保留缓冲，不释放或覆盖。
 */
static esp_err_t WaitTransfer(void)
{
    /* 没有成功提交的传输时不等待，避免等待永远不会到达的回调。
     * 信号量只有 DMA 完成回调释放，不用延时猜测传输是否结束。
     * 超时保留 pending 状态，上层必须停止后续绘制。
     * 此函数只由显示任务调用，状态无需跨任务互斥。 */
    if (s_pending)
    {
        ESP_RETURN_ON_FALSE(
            pdTRUE == xSemaphoreTake(s_done, pdMS_TO_TICKS(LCD_TIMEOUT_MS)),
            ESP_ERR_TIMEOUT, kTag, "DMA timeout; buffer retained");
        s_pending = false;
    }

    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 等待旧事务后提交一条；只在提交成功时标记等待完成。
 * @param y 起始行，按 STRIP_ROWS 递增。
 * @param pixels 有效内部 DMA 缓冲，完成前不得修改。
 * @return ESP_OK 或驱动错误；同一任务串行调用。
 */
static esp_err_t SubmitStrip(int y, const uint8_t *pixels)
{
    /* SPI 面板的命令传输和颜色传输共用同一总线。
     * 等待上一条后再设置绘制窗口，窗口不会覆盖在途命令。
     * 右下坐标是排除边界，必须用 y 加条高而非减一。
     * 提交成功才切换缓冲索引，失败不会伪造完成责任。 */
    ESP_RETURN_ON_ERROR(WaitTransfer(), kTag, "wait previous strip");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, y, PREVIEW_WIDTH,
                                                  y + STRIP_ROWS, pixels),
                        kTag, "draw strip");
    s_pending = true;
    s_next_strip ^= 1U;
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 初始化无独立复位脚的 ST7789 横屏显示链路。
 * @return ESP_OK 或初始化错误；失败分配保持至重启，不进入绘制。
 */
esp_err_t LcdInit(void)
{
    /* 背光此时保持关闭，初始化命令不会显示随机 GRAM 内容。
     * 同步信号量和缓冲在整个预览期间保持有效。
     * 初始化失败不会进入帧绘制，不访问未创建的面板。
     * 直连 LCD reset 不存在，面板通过命令执行软件复位。 */
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(NULL != s_done, ESP_ERR_NO_MEM, kTag,
                        "completion semaphore");

    /* DMA 缓冲必须在内部 RAM，避免 SPI 对 PSRAM 的隐式临时拷贝。 */
    for (unsigned i = 0; i < 2; ++i)
    {
        s_strips[i] = heap_caps_calloc(1, STRIP_BYTES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(NULL != s_strips[i], ESP_ERR_NO_MEM, kTag,
                            "strip allocation");
    }

    spi_bus_config_t bus = {
        .mosi_io_num = 16,
        .miso_io_num = 17,
        .sclk_io_num = 15,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = STRIP_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        kTag, "SPI init");
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t config = {
        .cs_gpio_num = 47,
        .dc_gpio_num = 48,
        .spi_mode = 0,
        .pclk_hz = LCD_SPI_HZ,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = TransferDone,
        .user_ctx = s_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)SPI2_HOST, &config, &io),
                        kTag, "panel IO");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel),
                        kTag, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), kTag, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), kTag,
                        "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), kTag, "swap XY");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), kTag,
                        "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag,
                        "display on");
    ESP_LOGI(kTag, "ST7789 320x240 RGB565 BE SPI=%d Hz", LCD_SPI_HZ);
    ESP_LOGI(kTag, "horizontal correction: reverse RGB565 pixels per row");
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 逐行拷贝摄像头大端像素并提交，覆盖整个屏幕。
 * @param frame 非空采集帧，调用者保证长度满足 stride × 240。
 * @param stride 每行有效跨度，至少 640 字节。
 * @return ESP_OK 或错误；最后一条完成后才成功返回。
 */
esp_err_t LcdDrawFrame(const uint8_t *frame, size_t stride)
{
    /* 输入长度在相机层验证，显示层再次检查指针和每行跨度。
     * 每条固定 40 行，240 行整除，因此没有尾部短条。
     * 反向像素拷贝只纠正水平镜像，不改变屏幕横屏坐标。
     * 最后一条完成前不会成功返回，不靠固定时间等待屏幕。 */
    ESP_RETURN_ON_FALSE((NULL != frame) && (stride >= (PREVIEW_WIDTH * 2)),
                        ESP_ERR_INVALID_ARG, kTag, "invalid frame");

    for (int y = 0; y < PREVIEW_HEIGHT; y += STRIP_ROWS)
    {
        /* 本次缓冲和在途缓冲交替使用；拷贝可与上一条 DMA 重叠。
         * 相机输出和 LCD 传输同为大端，不能再次交换字节。 */
        uint8_t *strip = s_strips[s_next_strip];

        for (int row = 0; row < STRIP_ROWS; ++row)
        {
            /* 最新修改：用户确认左右颠倒；传感器镜像位写入未生效，
             * 在条缓冲中按完整像素反向拷贝，保留每个像素大端字节序。
             * 源帧只读，不额外分配全帧，不影响摄像头 DMA 所有权。 */
            const uint8_t *source = frame + ((y + row) * stride);
            uint8_t *destination = strip + (row * PREVIEW_WIDTH * 2);

            for (int x = 0; x < PREVIEW_WIDTH; ++x)
            {
                int source_offset = (PREVIEW_WIDTH - 1 - x) * 2;
                destination[x * 2] = source[source_offset];
                destination[(x * 2) + 1] = source[source_offset + 1];
            }
        }

        ESP_RETURN_ON_ERROR(SubmitStrip(y, strip), kTag, "submit frame");
    }

    return WaitTransfer();
}

/**
 * @authors Codex
 * @brief 将主机 RGB565 值按大端编码填充诊断画面。
 * @param rgb565 单色颜色；不保留外部内存。
 * @return ESP_OK 或错误；DMA 超时后不可再次调用绘制。
 */
esp_err_t LcdFill(uint16_t rgb565)
{
    /* 诊断填充复用同一套条缓冲和完成机制，不走另一个 DMA 通路。
     * RGB565 整数拆成高低字节，避免主机小端导致颜色错误。
     * 双缓冲按提交顺序轮换，上一条 DMA 不会读到本次填充值。
     * 此接口只用于启动或相机故障，DMA 超时后不再调用。 */
    for (int y = 0; y < PREVIEW_HEIGHT; y += STRIP_ROWS)
    {
        uint8_t *strip = s_strips[s_next_strip];

        for (int offset = 0; offset < STRIP_BYTES; offset += 2)
        {
            strip[offset] = (uint8_t)(rgb565 >> 8);
            strip[offset + 1] = (uint8_t)rgb565;
        }

        ESP_RETURN_ON_ERROR(SubmitStrip(y, strip), kTag, "fill strip");
    }

    return WaitTransfer();
}
