/**
 * @file application.c
 * @author Codex
 * @brief 本地实时预览调度、帧统计与有限次数摄像头恢复。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：菜单归还借用帧后停流，独立轮询输入/回放，返回取景复用缓冲启流。
 * @note 法律信息：None；RGB565 原生输出代替设计初稿的软件 YUV 转换。
 */
#include "application.h"

#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "boards/atk_box3/board.h"
#include "camera/camera_capture.h"
#include "display/lcd_display.h"
#include "storage/camera_storage.h"

/* 时间用微秒，统计周期 1 秒；摄像头恢复总次数限定为 3。 */
#define STATS_PERIOD_US       (1000000)
#define MAX_RECOVERY_ATTEMPTS (3)
#define MENU_POLL_MS          (10)
/* 三缓冲恢复后完整丢弃一轮，避免将启流边界的过渡图像提交LCD。 */
#define PREVIEW_RESUME_DROP_FRAMES (3)
static const char *kTag = "preview";

/**
 * @authors Codex
 * @brief 摄像头异常时在所有帧归还后执行停流和重建。
 * @param bus 有效共享 I2C。
 * @param attempts 非空累计次数，成功后不清零，避免无限重试。
 * @return ESP_OK 或恢复错误；不释放 LCD 资源。
 */
static esp_err_t RecoverCamera(i2c_master_bus_handle_t bus, unsigned *attempts)
{
    /* 先停止采集 DMA，不能带着上一轮队列直接复位传感器。
     * CameraStop 关闭设备和组件，调用者不单独释放映射内存。
     * 停止失败意味着硬件可能仍在访问缓冲，立即向上返回。
     * 主控制任务串行重建，共用 I2C 总线保持存活。
     * LCD 在恢复期间保留最后一张完整画面，不重建 SPI 总线。 */
    esp_err_t result = CameraStop();

    if (ESP_OK != result)
    {
        return result;
    }

    while (*attempts < MAX_RECOVERY_ATTEMPTS)
    {
        /* 累计次数跨成功恢复保留，防止间歇故障无限占用启动任务。
         * 延时单位为毫秒，通过 RTOS 阻塞让出 CPU。
         * 重建复用原 I2C，避免再次创建相同控制端口。
         * 每次重建都会执行完整传感器复位和帧队列初始化。
         * 只有 CameraInit 完全成功才重新进入预览状态。 */
        ++(*attempts);
        ESP_LOGW(kTag, "state=RECOVERING attempt=%u", *attempts);
        vTaskDelay(pdMS_TO_TICKS(200));
        result = CameraInit(bus);

        if (ESP_OK == result)
        {
            ESP_LOGI(kTag, "state=PREVIEWING recovery complete");
            return ESP_OK;
        }

        esp_err_t cleanup = CameraStop();

        if (ESP_OK != cleanup)
        {
            return cleanup;
        }
    }

    return result == ESP_OK ? ESP_FAIL : result;
}

/**
 * @authors Codex
 * @brief 启流后丢弃三帧，保持此前已显示的黑屏，不读取或显示像素。
 * @details 每帧即使valid=false也须归还，避免预热耗尽采集队列。
 * 三次取帧分别受一秒超时约束，不用固定sleep猜测DMA完成。
 * 只在无其他借用帧时调用，返回失败由上层关闭设备并有限恢复。
 * 此函数不处理按键、不访问TF；预热结束后主循环继续正常输入处理。
 * 丢弃帧单独记录，不混入传输错误/无效帧统计。
 * @return ESP_OK三帧已全部归还；其他为取帧或归还错误。
 */
static esp_err_t WarmupPreview(void)
{
    for (unsigned index = 0; index < PREVIEW_RESUME_DROP_FRAMES; ++index)
    {
        CameraFrame frame = {0};
        esp_err_t result = CameraGet(&frame);

        if (ESP_OK != result)
        {
            return result;
        }

        /* 先保留只读诊断值，归还后frame会清零；不读取过渡帧像素。
         * 所有丢弃均必须实际QBUF成功，再记录预热进度。 */
        unsigned buffer_index = frame.index;
        bool valid = frame.valid;
        result = CameraReturn(&frame);

        if (ESP_OK != result)
        {
            return result;
        }

        ESP_LOGI(kTag, "PREVIEW_WARMUP discarded=%u buffer=%u valid=%d",
                 index + 1, buffer_index, valid);
    }

    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 最多替换两次已经完成的旧帧，不无限排队或忙轮询。
 * @param frame 当前借用帧，成功时变为最新可用帧。
 * @param dropped 非空丢帧统计输出。
 * @return ESP_OK 或错误；所有替换帧及时归还。
 */
static esp_err_t PreferLatest(CameraFrame *frame, uint32_t *dropped)
{
    /* 函数进入时已经借用一帧，短暂把取帧等待改为立即返回。
     * 只有一个采集调用任务，不会改变其他任务的等待行为。
     * 返回前恢复正常等待，保持低 CPU 占用和无帧故障检测。
     * dropped 记录主动丢弃旧帧，不把它误称为传输错误。
     * 输出 frame 的归还责任随令牌转移，不能丢失该责任。 */
    esp_err_t result = CameraSetTimeout(0);

    if (ESP_OK != result)
    {
        return result;
    }

    for (unsigned i = 0; i < 2; ++i)
    {
        /* 三缓冲最多检查另外两张已完成帧，不无限追赶新数据。
         * newer 从零初始化，失败时不使用未取得的像素指针。
         * 没有新帧属于正常情况，显示当前已持有的帧。
         * 新帧无效时将它归还，仍可保留旧的有效帧。
         * 替换有效帧前先归还旧帧，避免队列缓冲泄漏。 */
        CameraFrame newer = {0};
        result = CameraGet(&newer);

        if (ESP_ERR_TIMEOUT == result)
        {
            result = ESP_OK;
            break;
        }

        if (ESP_OK != result)
        {
            break;
        }

        /* 新帧异常时保留旧帧；两种分支都归还被丢弃的帧。 */
        if (!newer.valid)
        {
            result = CameraReturn(&newer);
            ++(*dropped);
        }
        else
        {
            result = CameraReturn(frame);

            if (ESP_OK != result)
            {
                CameraReturn(&newer);
                break;
            }

            *frame = newer;
            ++(*dropped);
        }

        if (ESP_OK != result)
        {
            break;
        }
    }

    esp_err_t restore = CameraSetTimeout(1000);
    return ESP_OK == result ? restore : result;
}

/**
 * @authors Codex
 * @brief 自动初始化并显示相机画面，持续统计完整屏幕传输帧率。
 * @return None；致命错误退出循环并保留串口诊断。
 * @note 主任务独占V4L2、文件和LCD；按键/I2S任务使用驱动同步。
 */
void ApplicationStart(void)
{
    /* 首帧计时从应用入口开始，不包含 ROM 和 bootloader 初始化。
     * 日志行还包含复位以来的时间，便于区分两种启动计时。
     * 外设仅在本任务创建，DMA 回调不执行耗时初始化。
     * 所有跨帧统计量初始化，避免首次报告读到未定义数据。
     * 启动失败保持串口诊断，不自动循环复位整板。 */
    int64_t startup = esp_timer_get_time();
    i2c_master_bus_handle_t bus = NULL;
    unsigned recovery_attempts = 0;
    ESP_LOGI(kTag, "camera-storage v2.8-resume-black state=INITIALIZING");
    ESP_LOGI(kTag, "PSRAM capacity=%u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    esp_err_t result = BoardInit(&bus);
    /* 板级初始化验证扩展器 ID 后才设置供电。
     * 未成功时禁止访问 LCD 或摄像头，避免使用无效总线。
     * 共享 I2C 的所有权在板级模块，应用只借用句柄。
     * 供电、电平与方向读回错误通过同一结果传播。
     * 人工重新启动前不反复切换未知硬件的电源状态。 */

    if (ESP_OK != result)
    {
        ESP_LOGE(kTag, "state=ERROR board=%s", esp_err_to_name(result));
        return;
    }

    result = BoardInitSpi();

    if (ESP_OK != result)
    {
        ESP_LOGE(kTag, "state=ERROR SPI=%s", esp_err_to_name(result));
        return;
    }

    /* 最新修改：先初始化 TF 的 SPI 模式，再发送 LCD 初始化命令。
     * TF缺失或挂载失败仍允许取景，格式化须实体键双确认。 */
    StorageInit(bus);
    result = LcdInit();
    /* LCD 必须先初始化成功，之后才可调用填充和帧绘制。
     * 初始化失败不继续创建采集缓冲，便于定位故障阶段。
     * 屏幕方向配置由板级布局决定，相机左右修正另行处理。
     * DMA 条缓冲归显示模块持有，应用不会直接修改它们。
     * SPI由屏幕与TF共用，主任务按顺序访问两个设备。 */

    if (ESP_OK != result)
    {
        ESP_LOGE(kTag, "state=ERROR LCD=%s", esp_err_to_name(result));
        return;
    }

    /* 开机先短暂红、绿、蓝色图，供实物观察方向/颜色及 LCD 通路。 */
    const uint16_t colors[3] = {0xf800, 0x07e0, 0x001f};

    for (unsigned i = 0; i < 3; ++i)
    {
        /* RGB565 单色值采用主机整数，LCD 按大端发送。
         * 完整诊断图传输完成后才亮背光，避免随机显存闪烁。
         * 每种颜色停留 100 ms，便于重启时观察显示通路。
         * 填充失败不继续访问在途缓冲，保留错误日志。
         * 诊断图不经过摄像头，能帮助区分采集和显示故障。 */
        result = LcdFill(colors[i]);

        if (ESP_OK != result)
        {
            ESP_LOGE(kTag, "state=ERROR LCD color test=%s",
                     esp_err_to_name(result));
            return;
        }

        result = BoardSetBacklight(true);

        if (ESP_OK != result)
        {
            ESP_LOGE(kTag, "state=ERROR backlight=%s", esp_err_to_name(result));
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    result = CameraInit(bus);

    if (ESP_OK != result)
    {
        result = RecoverCamera(bus, &recovery_attempts);
    }

    if (ESP_OK != result)
    {
        ESP_LOGE(kTag, "state=ERROR camera init=%s", esp_err_to_name(result));
        LcdFill(0xf800);
        return;
    }

    int64_t window = esp_timer_get_time();
    /* shown 与 display_sum 属于同一个统计窗口。
     * 仅完整显示且归还成功后才累加帧数。
     * total 跨窗口增长，用于判断预览是否持续推进。
     * last_valid 独立记录成功画面时间，检测持续坏帧。
     * first 只标记本次应用启动，恢复过程不重复伪造首帧。 */
    uint32_t shown = 0;
    uint32_t dropped = 0;
    uint32_t total = 0;
    int64_t display_sum = 0;
    int64_t last_valid = window;
    bool first = true;
    /* 非零表示恢复取景的黑屏已实际刷完，直到首个正常预览绘制成功。 */
    int64_t resume_black_since = 0;
    ESP_LOGI(kTag, "state=PREVIEWING");

    while (true)
    {
        if (!StorageNeedsCapture())
        {
            /* 初始格式提示或拍照后进入提示时，也必须先停止DMA。
             * 此处处于两次取帧之间，不持有帧，不释放设备映射内存。
             * 菜单输入、卡状态和媒体回放时钟均不再等待CameraGet。 */
            result = CameraPause();

            if (ESP_OK != result)
            {
                ESP_LOGE(kTag, "state=ERROR menu pause=%s",
                         esp_err_to_name(result));
                StorageStop();
                break;
            }

            StorageProcess(NULL);

            if (!StorageNeedsCapture())
            {
                result = StorageDraw(NULL);

                if (ESP_OK != result)
                {
                    ESP_LOGE(kTag, "state=ERROR menu draw=%s",
                             esp_err_to_name(result));
                    StorageStop();
                    break;
                }

                /* idle统计与采集FPS区分；captures在菜单期间必须不变。
                 * 保留STATS格式使已有只读验收脚本可检查堆稳定性。
                 * 文件回放会更新LCD，但不能把菜单轮询算作摄像头帧。 */
                int64_t idle_now = esp_timer_get_time();

                if ((idle_now - window) >= STATS_PERIOD_US)
                {
                    ESP_LOGI(kTag,
                             "MENU_IDLE streaming=%d captures=%lu poll_ms=%d",
                             CameraIsStreaming(),
                             (unsigned long)CameraGetCount(), MENU_POLL_MS);
                    ESP_LOGI(
                        kTag,
                        "STATS fps=0.00 display_ms=0.00 total=%" PRIu32
                        " dropped=%" PRIu32
                        " crc=00000000 internal_free=%u psram_free=%u",
                        total, dropped,
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                    window = idle_now;
                }

                /* 最多每10ms轮询一次，空闲时让出CPU，不靠相机DMA唤醒。
                 * 卡扫描/回放及LCD传输依旧同步，其耗时不属于固定轮询等待。 */
                vTaskDelay(pdMS_TO_TICKS(MENU_POLL_MS));
                continue;
            }
        }

        if (!CameraIsStreaming())
        {
            /* STREAMOFF已清空队列，恢复必须重新排入全部三个缓冲。
             * 启流失败关闭设备并有限恢复，不无限重复失败的部分排队。
             * 重设统计窗口和有效帧时钟，菜单停留不算采集超时。 */
            /* 最新修改：先完成全黑LCD DMA，再启流和预热。
             * 黑屏不依赖相机帧，也不保留可能仍被DMA覆盖的旧帧指针。
             * 任一黑屏绘制错误都禁止启流后显示未知过渡图像。 */
            result = LcdFill(0x0000);

            if (ESP_OK != result)
            {
                ESP_LOGE(kTag, "state=ERROR resume black=%s",
                         esp_err_to_name(result));
                break;
            }

            resume_black_since = esp_timer_get_time();
            ESP_LOGI(kTag, "PREVIEW_RESUME_BLACK shown warmup_frames=%d",
                     PREVIEW_RESUME_DROP_FRAMES);
            result = CameraResume();

            if (ESP_OK != result)
            {
                ESP_LOGW(kTag, "camera resume failed=%s",
                         esp_err_to_name(result));
                result = RecoverCamera(bus, &recovery_attempts);
            }

            if (ESP_OK != result)
            {
                ESP_LOGE(kTag, "state=ERROR resume=%s",
                         esp_err_to_name(result));
                break;
            }

            result = WarmupPreview();

            if (ESP_OK != result)
            {
                /* 预热取帧失败保持黑屏，重建后也重新丢弃过渡帧。
                 * 使用既有累计恢复上限，失败不会无限阻塞在黑屏。 */
                ESP_LOGW(kTag, "preview warmup failed=%s",
                         esp_err_to_name(result));
                result = RecoverCamera(bus, &recovery_attempts);

                if (ESP_OK == result)
                {
                    result = WarmupPreview();
                }
            }

            if (ESP_OK != result)
            {
                ESP_LOGE(kTag, "state=ERROR preview warmup=%s",
                         esp_err_to_name(result));
                break;
            }

            window = esp_timer_get_time();
            last_valid = window;
            shown = 0;
            display_sum = 0;
        }

        /* 等待完整相机帧，不读摄像头 DMA 正在写入的缓冲。
         * 取到帧后检查已完成新帧，优先降低画面积压延迟。
         * 同一循环最多临时持有两帧，替换后立即归还一帧。
         * 取帧有一秒上限，不会永久堵塞故障处理路径。
         * 正常等待和 DMA 同步都会让出 CPU，不采用忙轮询。 */
        CameraFrame frame = {0};
        result = CameraGet(&frame);

        if (ESP_OK == result)
        {
            result = PreferLatest(&frame, &dropped);
        }

        if ((ESP_OK == result) && (!frame.valid))
        {
            /* 无效长度或错误标志的帧禁止送到屏幕。
             * 这张帧仍有借用令牌，丢弃时必须归还驱动。
             * 短暂坏帧容许丢弃，连续一秒无有效帧则恢复。
             * 不完整图像不能按固定尺寸读取，防止内存越界。
             * 归还失败也进入故障处理，不耗尽剩余缓冲。 */
            result = CameraReturn(&frame);
            ++dropped;

            if ((ESP_OK == result)
                && ((esp_timer_get_time() - last_valid) < STATS_PERIOD_US))
            {
                continue;
            }

            result = ESP_ERR_INVALID_RESPONSE;
        }

        if (ESP_OK != result)
        {
            /* 有借用帧时先归还，重建前停止驱动，错误不绕过生命周期。 */
            if (NULL != frame.data)
            {
                CameraReturn(&frame);
            }

            ESP_LOGW(kTag, "capture error=%s", esp_err_to_name(result));
            /* 恢复前正常封闭已有视频，避免停止采集后遗留打开的文件。 */
            StorageStop();
            result = RecoverCamera(bus, &recovery_attempts);

            if (ESP_OK != result)
            {
                ESP_LOGE(kTag, "state=ERROR recovery exhausted=%s",
                         esp_err_to_name(result));
                LcdFill(0xf800);
                break;
            }

            window = esp_timer_get_time();
            last_valid = window;
            shown = 0;
            display_sum = 0;
            continue;
        }

        /* CRC 每个统计周期一次，便于识别固定/变化帧，不逐帧打印。
         * 它证明采集数据变化，不能代替肉眼判断颜色和屏幕画面。 */
        int64_t begin = esp_timer_get_time();
        bool report = (begin - window) >= STATS_PERIOD_US;
        uint32_t crc =
            report ? esp_rom_crc32_le(0, frame.data, frame.length) : 0;
        /* 最新修改：主任务先处理三键及存储状态，再按页面绘制。
         * 音频/按键独立任务不持有摄像头帧，不并发写卡或画屏。
         * 所有LCD调用都等待最后DMA，文件与显示共用SPI按序访问。 */
        StorageProcess(&frame);

        if (!StorageNeedsCapture())
        {
            /* 菜单动作先封闭录像，再归还最后一帧；停流前不绘制菜单。
             * LCD绘制不借用相机像素，暂停后可以独立进行媒体扫描/回放。
             * 归还或停流失败保留资源，不在错误状态继续访问图像。 */
            result = CameraReturn(&frame);

            if (ESP_OK == result)
            {
                result = CameraPause();
            }

            if (ESP_OK == result)
            {
                result = StorageDraw(NULL);
            }

            if (ESP_OK != result)
            {
                ESP_LOGE(kTag, "state=ERROR enter menu=%s",
                         esp_err_to_name(result));
                StorageStop();
                break;
            }

            resume_black_since = 0;
            window = esp_timer_get_time();
            shown = 0;
            display_sum = 0;
            continue;
        }

        result = StorageDraw(&frame);
        /* 成功返回时最后一条 LCD DMA 已完成。
         * LCD 不长期持有原帧，原帧只在分条拷贝时读取。
         * 即使绘制失败，在途 LCD 访问的也是独立条缓冲。
         * 因此可以归还相机帧，但不得重新覆盖 LCD 条缓冲。
         * 计时覆盖本帧软件处理和传输，不是实际曝光延迟。 */
        int64_t end = esp_timer_get_time();
        esp_err_t returned = CameraReturn(&frame);

        if ((ESP_OK != result) || (ESP_OK != returned))
        {
            ESP_LOGE(kTag,
                     "state=ERROR draw=%s return=%s; DMA buffers retained",
                     esp_err_to_name(result), esp_err_to_name(returned));
            CameraStop();
            StorageStop();
            break;
        }

        if (resume_black_since > 0)
        {
            /* 只有正常取景已完成LCD DMA且帧已归还，才宣布黑屏结束。 */
            ESP_LOGI(kTag,
                     "PREVIEW_RESUME_SHOWN warmup_frames=%d black_us=%" PRId64,
                     PREVIEW_RESUME_DROP_FRAMES, end - resume_black_since);
            resume_black_since = 0;
        }

        /* 最新修改：相机没有借用帧时才允许改变分辨率和缓冲。 */
        result = StorageCapturePending(bus);

        if (ESP_OK != result)
        {
            StorageStop();
            result = RecoverCamera(bus, &recovery_attempts);

            if (ESP_OK != result)
            {
                break;
            }
        }

        last_valid = esp_timer_get_time();
        ++shown;
        ++total;
        display_sum += end - begin;

        if (first)
        {
            /* 首帧报告仅输出一次；多次出现可提示设备重启。
             * 此时屏幕传输完成和相机缓冲归还均已成功。
             * 后续 FPS 统计不重复写入启动成功时间。
             * 字节数对应当前固定 QVGA RGB565 有效帧尺寸。 */
            ESP_LOGI(kTag, "FIRST_FRAME startup_ms=%" PRId64 " bytes=%d",
                     (end - startup) / 1000, PREVIEW_FRAME_BYTES);
            first = false;
        }

        if (report)
        {
            /* FPS 使用实际窗口时间，不假定精确经过一秒。
             * display_ms 包含当期 CRC 计算及最后一条传输等待。
             * 内部 RAM 与 PSRAM 分别记录，便于检查不同内存池。
             * 成功报告后清零窗口量，不清零累计显示和丢帧数。 */
            ESP_LOGI(kTag,
                     "STATS fps=%.2f display_ms=%.2f total=%" PRIu32
                     " dropped=%" PRIu32 " crc=%08" PRIx32
                     " internal_free=%u psram_free=%u",
                     ((double)shown * STATS_PERIOD_US) / (end - window),
                     (double)display_sum / (shown * 1000), total, dropped, crc,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            shown = 0;
            display_sum = 0;
            window = end;
        }
    }
}
