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
#include <stdio.h>
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "boards/atk_box3/board.h"
#include "lcd_font.h"

/* 实测 16 行条带受任务唤醒开销限制；改 40 行，双缓冲合计 50 KiB。 */
#define STRIP_ROWS     (40)
#define STRIP_BYTES    (PREVIEW_WIDTH * STRIP_ROWS * 2)
#define LCD_TIMEOUT_MS (1000)
#define LCD_SPI_HZ     (60000000)

static bool s_flash = false; /* 快门反馈只作用于 LCD，禁止修改保存像素。 */
static char s_caption[40] = {0};
static const char *kTag = "lcd";
static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_done = NULL;
/* 任务私有状态：最多一个 DMA 事务在途，超时保持 pending 防止覆盖。 */
static uint8_t *s_strips[2] = {NULL, NULL};
static bool s_pending = false;
static unsigned s_next_strip = 0;

/**
 * @authors Codex
 * @brief 使用共享点阵字体绘制当前条带，保持原菜单坐标。
 * @param strip 非空且不在DMA传输中的内部条缓冲。
 * @param strip_y 当前条带屏幕起始行，像素。
 * @param text 非空ASCII文字，不持有调用者指针。
 * @param text_y 文字在全屏中的纵坐标，像素。
 * @return None，自动裁剪条带外的部分。
 */
static void DrawText(uint8_t *strip, int strip_y, const char *text, int text_y)
{
    LcdFontDraw(strip, PREVIEW_WIDTH, STRIP_ROWS, 4, text_y - strip_y, text, 2,
                0xffff);
}

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

    /* 最新修改：SPI2 由 BoardInitSpi 创建，TF 先进入 SPI 模式。
     * LCD 只创建设备，不再重复创建总线，保留原 DMA 条带策略。 */
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
 * @param recording true 时绘制红色录像标记，仅改变 LCD 条缓冲。
 * @return ESP_OK 或错误；最后一条完成后才成功返回。
 */
static esp_err_t DrawPixels(const uint8_t *frame, size_t stride, bool recording,
                            bool mirror)
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
                int source_offset = (mirror ? (PREVIEW_WIDTH - 1 - x) : x) * 2;
                destination[x * 2] = source[source_offset];
                destination[(x * 2) + 1] = source[source_offset + 1];
            }

            /* 最新修改：右上角 12 像素红方块提示录像状态。
             * 仅修改显示条缓冲，保存照片和视频不会出现此标记。
             * 四像素边距使状态标记不贴屏幕边缘。
             * 行复制结束后覆盖，避免正常预览逐像素额外判断。 */
            if (recording && ((y + row) >= 4) && ((y + row) < 16))
            {
                for (int x = PREVIEW_WIDTH - 16; x < PREVIEW_WIDTH - 4; ++x)
                {
                    destination[x * 2] = 0xf8;
                    destination[(x * 2) + 1] = 0x00;
                }
            }
        }

        /* 最新修改：标题仅叠加到显示缓冲，照片/录像原像素不受影响。 */
        if ((y == 0) && (s_caption[0] != '\0'))
        {
            memset(strip, 0, PREVIEW_WIDTH * 24 * 2);
            DrawText(strip, y, s_caption, 4);

            /* 标题背景覆盖后重新绘制红色REC标记，避免被黑色标题遮住。
             * 标记位于文本最右侧外边距，仍不修改存储像素。 */
            if (recording)
            {
                for (unsigned row = 4; row < 16; ++row)
                {
                    for (unsigned x = PREVIEW_WIDTH - 16; x < PREVIEW_WIDTH - 4;
                         ++x)
                    {
                        unsigned offset = (row * PREVIEW_WIDTH + x) * 2;
                        strip[offset] = 0xf8;
                        strip[offset + 1] = 0;
                    }
                }
            }
        }

        /* 最新修改：将整幅显示与白色混合四分之一，保留画面内容。 */
        if (s_flash)
        {
            for (unsigned offset = 0; offset < STRIP_BYTES; offset += 2)
            {
                uint16_t pixel =
                    ((uint16_t)strip[offset] << 8) | strip[offset + 1];
                unsigned red = (pixel >> 11) & 31;
                unsigned green = (pixel >> 5) & 63;
                unsigned blue = pixel & 31;
                pixel = ((red + ((31 - red + 3) / 4)) << 11)
                        | ((green + ((63 - green + 3) / 4)) << 5)
                        | (blue + ((31 - blue + 3) / 4));
                strip[offset] = pixel >> 8;
                strip[offset + 1] = (uint8_t)pixel;
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

/**
 * @authors Codex
 * @brief 预览保持原方向修正和录像标记，文本仅用于显示。
 * @param frame 非空大端RGB565帧。
 * @param stride 每行跨度，字节。
 * @param recording true绘制录像标记。
 * @return ESP_OK或传输错误。
 */
esp_err_t LcdDrawFrame(const uint8_t *frame, size_t stride, bool recording)
{
    return DrawPixels(frame, stride, recording, true);
}

/**
 * @authors Codex
 * @brief 回放图像已按文件方向解码，不再次水平镜像。
 * @param pixels 非空RGB565大端全屏缓冲。
 * @return ESP_OK或传输错误。
 */
esp_err_t LcdDrawImage(const uint8_t *pixels)
{
    return DrawPixels(pixels, PREVIEW_WIDTH * 2, false, false);
}

/**
 * @authors Codex
 * @brief 复制模式标题到模块内部，避免持有临时栈字符串。
 * @param caption 非空字符串，最大39字符。
 * @return None。
 */
void LcdSetCaption(const char *caption)
{
    snprintf(s_caption, sizeof(s_caption), "%s", caption);
}

/**
 * @authors Codex
 * @brief 绘制最多8行ASCII菜单，所有条带仍由完成信号保护。
 * @param lines 非空换行分隔文本，各行最多26个可见字符。
 * @return ESP_OK或LCD错误。
 */
esp_err_t LcdShowText(const char *lines)
{
    for (int y = 0; y < PREVIEW_HEIGHT; y += STRIP_ROWS)
    {
        uint8_t *strip = s_strips[s_next_strip];

        /* 背景RGB565深蓝0x0008，文字白色，避免浅背景阅读困难。
         * 所有文字逐行复制至有界临时缓冲，不动态申请内存。
         * 最后一条DMA完成后才允许存储访问共用SPI总线。 */
        for (unsigned pixel = 0; pixel < STRIP_BYTES; pixel += 2)
        {
            strip[pixel] = 0;
            strip[pixel + 1] = 8;
        }

        const char *begin = lines;

        for (unsigned line = 0; (line < 8) && (*begin != '\0'); ++line)
        {
            char text[40] = {0};
            size_t length = strcspn(begin, "\n");
            size_t count = (length >= sizeof(text)) ? sizeof(text) - 1 : length;
            memcpy(text, begin, count);
            DrawText(strip, y, text, 8 + line * 28);
            begin += length;

            if (*begin == '\n')
            {
                ++begin;
            }
        }

        ESP_RETURN_ON_ERROR(SubmitStrip(y, strip), kTag, "menu strip");
    }

    return WaitTransfer();
}

/**
 * @authors Codex
 * @brief 设置当前显示的快门白色反馈，仅主任务调用。
 * @param enabled true 启用白色混合。
 * @return None。
 */
void LcdSetFlash(bool enabled)
{
    s_flash = enabled;
}
