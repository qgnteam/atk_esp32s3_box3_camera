/**
 * @file camera_audio.c
 * @author Codex
 * @brief 音频任务隔离写卡延迟，录音溢出可检测，回放共享时钟。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增ES7210/ES8311控制；法律信息：None。
 * @note 使用Espressif esp_codec_dev驱动，保留组件原许可证。
 */
#include "camera_audio.h"
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "boards/atk_box3/board.h"

/* 20ms一次读取；4秒环形缓冲能覆盖正常单帧写卡延迟。
 * DMA队列16块共320ms，优先级8的采集任务只复制和计算统计。
 * 文件写入和I2C编解码器配置均不在中断或采集任务中执行。
 * RX/TX按模式创建并删除，防止TDM与标准I2S同时争用时钟。 */
#define AUDIO_BLOCK_SAMPLES (480)
#define AUDIO_RING_SAMPLES  (AUDIO_SAMPLE_RATE * 4)
#define AUDIO_TDM_SLOTS     (4)
static const char *kTag = "audio";
static const audio_codec_if_t *s_adc = NULL;
static const audio_codec_if_t *s_dac = NULL;
static i2s_chan_handle_t s_channel = NULL;
static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_done = NULL;
static int16_t *s_ring = NULL;
static int16_t *s_raw = NULL;
static size_t s_head = 0;
static size_t s_tail = 0;
static size_t s_count = 0;
static bool s_running = false;
static bool s_stop = false;
static bool s_playing = false;
static bool s_failed = false;
static volatile uint32_t s_dma_overflows = 0;
static CameraAudioStats s_stats = {0};
static const int16_t *s_play_pcm = NULL;
static size_t s_play_samples = 0;

/**
 * @authors Codex
 * @brief DMA队列溢出中断只计数，不阻塞、不打印、不申请内存。
 * @param channel 未使用的I2S句柄。
 * @param event 未使用事件数据。
 * @param context 未使用。
 * @return false，不请求额外任务唤醒。
 */
static bool AudioOverflow(i2s_chan_handle_t channel, i2s_event_data_t *event,
                          void *context)
{
    (void)channel;
    (void)event;
    (void)context;
    ++s_dma_overflows;
    return false;
}

/**
 * @authors Codex
 * @brief 独立采集任务从四个TDM槽提取MIC1，写有界环形缓冲。
 * @details
 * 任务只读I2S并写环形缓冲，不访问TF或LCD。
 * 原始TDM每帧包含四个16位槽，按完整帧数计算有效样本。
 * DMA队列长度独立于软件缓冲，两个溢出源都必须检查。
 * 读操作最多100ms，停止请求不会无限等待下一块。
 * 缓冲不足时明确置失败标志，不通过覆盖旧样本掩盖丢音。
 * 16位最小值先扩展到32位再取绝对值。
 * 平方和用64位累计，可覆盖最长60秒录像的满量程样本。
 * 四槽峰值和平方和用于检查物理通道及回声参考。
 * 诊断结果不自动更改选槽，避免选择浮空输入噪声。
 * 环形读写及统计使用同一互斥锁保护。
 * 主任务写卡不持有音频锁，采集不等待整个文件事务。
 * 停止时先释放完成信号，之后自删除且不再访问共享缓冲。
 * @param context 未使用。
 * @return None，停止后释放完成信号并自删除。
 */
static void CaptureTask(void *context)
{
    (void)context;

    while (true)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool stopped = s_stop;
        xSemaphoreGive(s_lock);

        if (stopped)
        {
            break;
        }

        size_t bytes = 0;
        esp_err_t result = i2s_channel_read(
            s_channel, s_raw, AUDIO_BLOCK_SAMPLES * AUDIO_TDM_SLOTS * 2, &bytes,
            100);
        size_t samples = bytes / (AUDIO_TDM_SLOTS * 2);
        xSemaphoreTake(s_lock, portMAX_DELAY);

        /* 短读按完整四槽样本处理；错误和溢出不会用静音掩盖。
         * 锁保护环形缓冲与统计；文件写入不持有该锁。
         * MIC1为槽0，其他槽用于硬件诊断，不进入AVI音轨。
         * 样本用32位绝对值，正确处理-32768而不溢出。 */
        if ((result != ESP_OK) && (result != ESP_ERR_TIMEOUT))
        {
            s_failed = true;
        }

        if (samples > (AUDIO_RING_SAMPLES - s_count))
        {
            ++s_stats.overflows;
            s_failed = true;
        }
        else
        {
            for (size_t sample = 0; sample < samples; ++sample)
            {
                /* 检查每个槽的能量，发现错误接线或回声参考选槽。
                 * 诊断不改变音轨选择，避免根据噪声自动切换麦克风。 */
                for (unsigned slot = 0; slot < AUDIO_TDM_SLOTS; ++slot)
                {
                    int32_t raw_value = s_raw[sample * AUDIO_TDM_SLOTS + slot];
                    uint32_t amplitude =
                        (raw_value < 0) ? -raw_value : raw_value;
                    if (amplitude > s_stats.slot_peak[slot])
                    {
                        s_stats.slot_peak[slot] = amplitude;
                    }
                    s_stats.slot_square_sum[slot] +=
                        (uint64_t)amplitude * amplitude;
                }

                int16_t value = s_raw[sample * AUDIO_TDM_SLOTS];
                int32_t magnitude = (value < 0) ? -(int32_t)value : value;
                s_ring[s_head] = value;
                s_head = (s_head + 1) % AUDIO_RING_SAMPLES;
                s_stats.peak = ((uint32_t)magnitude > s_stats.peak)
                                   ? (uint32_t)magnitude
                                   : s_stats.peak;
                s_stats.square_sum += (uint64_t)(magnitude * magnitude);
            }

            s_count += samples;
            s_stats.samples += samples;
        }

        xSemaphoreGive(s_lock);
    }

    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/**
 * @authors Codex
 * @brief 回放任务将单声道复制到左右声道，I2S时钟决定采样速率。
 * @details
 * PCM指针由浏览器拥有，任务不负责申请或释放。
 * 左右声道复制相同值，板上单扬声器仍得到完整单声道内容。
 * 播放位置只由本任务推进，主任务只设置停止请求。
 * 每次写入为最多480个样本，即24kHz下20ms。
 * I2S写入最多100ms，超时和短写都计为失败。
 * TX缓冲使用模块内部内存，不把PSRAM指针直接交给DMA。
 * 音频播放期间主任务可以读取和绘制视频。
 * 任务不配置I2C或切换功放，避免供电写改写竞争。
 * 最后留40ms让已排队数据播放完，避免切断尾部。
 * 完成信号意味着任务已放弃PCM，调用者可以随后释放。
 * @param context 未使用。
 * @return None，停止或播放结束后发出完成信号。
 */
static void PlaybackTask(void *context)
{
    (void)context;
    size_t position = 0;

    while (position < s_play_samples)
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool stopped = s_stop;
        xSemaphoreGive(s_lock);

        if (stopped)
        {
            break;
        }

        size_t count = s_play_samples - position;
        count = (count > AUDIO_BLOCK_SAMPLES) ? AUDIO_BLOCK_SAMPLES : count;

        /* 回放缓冲由浏览器持有，本任务退出前不能释放。
         * 使用独立TX缓冲，避免驱动读取正在修改的样本。
         * 同样20ms一块，停止等待有界，不持锁阻塞I2S写入。 */
        for (size_t sample = 0; sample < count; ++sample)
        {
            s_raw[sample * 2] = s_play_pcm[position + sample];
            s_raw[(sample * 2) + 1] = s_play_pcm[position + sample];
        }

        size_t written = 0;
        esp_err_t result =
            i2s_channel_write(s_channel, s_raw, count * 4, &written, 100);

        if ((ESP_OK != result) || (written != (count * 4)))
        {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_failed = true;
            xSemaphoreGive(s_lock);
            break;
        }

        position += count;
    }

    /* 等待最后DMA数据播放，停止请求最多多等待40ms。 */
    vTaskDelay(pdMS_TO_TICKS(40));
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/**
 * @authors Codex
 * @brief 初始化编解码器控制接口及永久缓冲；失败不允许无声录制。
 * @details
 * 先开启XIO12/13音频供电，再创建编解码器控制接口。
 * 功放初始化为关闭，录音不会主动输出声音形成反馈。
 * 录音环形缓冲存活至重启，避免每次录制大块内存碎片。
 * 原始采集及回放块复用内部RAM缓冲，二者不能同时运行。
 * ES7210控制地址为8位0x80，原生7位地址为0x40。
 * ES8311控制地址为8位0x30，原生7位地址为0x18。
 * 公共I2C驱动序列化按键、传感器和codec事务。
 * MIC1至MIC4初始化为启用，保证四槽TDM输出格式。
 * 驱动构造失败不能继续使用NULL接口函数指针。
 * 初始化失败保留诊断，录像层拒绝无声降级。
 * 编解码器实例只由主任务配置，音频任务只操作数据通道。
 * 原组件许可证随托管依赖保留，不复制修改驱动源码。
 * @param bus 有效共享I2C句柄，所有权保留在board。
 * @return ESP_OK或错误；已分配模块资源保持至重启。
 */
esp_err_t CameraAudioInit(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_ERROR(BoardEnableAudio(), kTag, "audio supply");
    ESP_RETURN_ON_ERROR(BoardSetAmplifier(false), kTag, "amplifier off");
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_ring = heap_caps_calloc(AUDIO_RING_SAMPLES, sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_raw = heap_caps_calloc(AUDIO_BLOCK_SAMPLES * AUDIO_TDM_SLOTS,
                             sizeof(int16_t), MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE((NULL != s_lock) && (NULL != s_done) && (NULL != s_ring)
                            && (NULL != s_raw),
                        ESP_ERR_NO_MEM, kTag, "audio allocation");

    /* esp_codec_dev使用八位地址，内部转换为七位，不能直接填0x40。
     * 各codec只拥有自己的I2C设备，不删除共享总线。
     * 时钟由I2S主机提供，MIC1~4启用以保持四槽TDM布局。 */
    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *adc_control =
        audio_codec_new_i2c_ctrl(&control);
    ESP_RETURN_ON_FALSE(NULL != adc_control, ESP_ERR_NO_MEM, kTag, "ADC I2C");
    es7210_codec_cfg_t adc = {
        .ctrl_if = adc_control,
        .master_mode = false,
        .mic_selected = 0x0f,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    s_adc = es7210_codec_new(&adc);
    ESP_RETURN_ON_FALSE(NULL != s_adc, ESP_FAIL, kTag, "ES7210 init");
    control.addr = ES8311_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *dac_control =
        audio_codec_new_i2c_ctrl(&control);
    ESP_RETURN_ON_FALSE(NULL != dac_control, ESP_ERR_NO_MEM, kTag, "DAC I2C");
    es8311_codec_cfg_t dac = {
        .ctrl_if = dac_control,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = true,
        .mclk_div = 256,
    };
    ESP_RETURN_ON_FALSE(NULL != dac.gpio_if, ESP_ERR_NO_MEM, kTag, "DAC GPIO");
    s_dac = es8311_codec_new(&dac);
    ESP_RETURN_ON_FALSE(NULL != s_dac, ESP_FAIL, kTag, "ES8311 init");
    ESP_LOGI(kTag, "READY ES7210=0x40 ES8311=0x18 PCM=24000/16/mono MIC1");
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 删除停止的I2S通道，调用前任务必须已经退出。
 * @return None，不处理仍被任务使用的句柄。
 */
static void DeleteChannel(void)
{
    if (NULL != s_channel)
    {
        i2s_channel_disable(s_channel);
        i2s_del_channel(s_channel);
        s_channel = NULL;
    }
}

/**
 * @authors Codex
 * @brief 创建RX TDM通道并启动采集，所有失败路径关闭通道。
 * @details
 * 播放和录制都检查s_running，禁止同时使用音频时钟。
 * RX为主机模式，GPIO21提供MCLK，GPIO38/39提供BCLK/WS。
 * GPIO41只接收ES7210数据，录制不驱动GPIO40音频输出。
 * MCLK为采样率乘256，24kHz时为6.144MHz。
 * 四个16位槽使用Philips TDM，保持与ADC输出一致。
 * 16个DMA描述符提供约320ms底层队列。
 * 软件环形缓冲在创建任务前重置，排除上一段音频残留。
 * 完成信号在启动前清空，不能误消费上一任务的结束事件。
 * codec配置或启用失败关闭通道，不创建录音任务。
 * 任务创建失败也撤销已经启用的I2S和ADC。
 * 只有完全成功后设置s_running，让上层据此确定生命周期。
 * 第一次实际采集由I2S DMA完成，不使用静音填充伪造样本。
 * @return ESP_OK或错误；只有音频初始化成功才可调用。
 */
esp_err_t CameraAudioRecordStart(void)
{
    if ((NULL == s_adc) || (NULL == s_dac) || s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 16;
    channel.dma_frame_num = AUDIO_BLOCK_SAMPLES;
    esp_err_t result = i2s_new_channel(&channel, NULL, &s_channel);

    if (ESP_OK != result)
    {
        return result;
    }

    i2s_tdm_config_t config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {.mclk = 21, .bclk = 38, .ws = 39, .dout = -1, .din = 41},
    };
    result = i2s_channel_init_tdm_mode(s_channel, &config);
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = 0x0f,
    };

    /* 先配置采样格式再启用ADC，任何codec失败都禁止创建视频。
     * 驱动已在new阶段初始化寄存器，set_fs使用该组件的公开接口。
     * 本次统计和环形缓冲在任务启动之前重置，无并发读写。 */
    if ((ESP_OK == result)
        && ((s_adc->set_fs(s_adc, &format) != 0)
            || (s_adc->set_mic_gain(s_adc, 30.0f) != 0)
            || (s_adc->enable(s_adc, true) != 0)))
    {
        result = ESP_FAIL;
    }

    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = AudioOverflow};

    if (ESP_OK == result)
    {
        result =
            i2s_channel_register_event_callback(s_channel, &callbacks, NULL);
    }

    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_stop = false;
    s_playing = false;
    s_failed = false;
    s_dma_overflows = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    xSemaphoreTake(s_done, 0);

    if (ESP_OK == result)
    {
        result = i2s_channel_enable(s_channel);
    }

    if ((ESP_OK == result)
        && (pdPASS
            != xTaskCreate(CaptureTask, "camera_pcm", 4096, NULL, 8, NULL)))
    {
        result = ESP_ERR_NO_MEM;
    }

    if (ESP_OK != result)
    {
        DeleteChannel();
        s_adc->enable(s_adc, false);
        return result;
    }

    s_running = true;
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 请求录音停止，最多等1秒；统计包括DMA和环形缓冲溢出。
 * @details
 * 停止请求通过互斥锁发布，读任务最多100ms检查一次。
 * 完成信号等待上限1秒，超时保留仍在使用的通道资源。
 * 只在确认任务结束后删除I2S RX句柄。
 * ADC在数据通道停止后关闭，避免采集任务读取失效接口。
 * 环形缓冲保留，供存储主任务写出最后一批PCM。
 * 统计输出是停止后的快照，不继续由任务更新。
 * DMA溢出计数加入软件溢出统计，不能遗漏底层丢音。
 * 峰值和平方和证明采样数据变化，不能代替人声试听。
 * 返回失败使存储层丢弃本段TMP，而非发布缺音文件。
 * 统计中的样本数仅包含已成功保留的完整单声道样本。
 * @param stats 非空输出，所有权在调用者。
 * @return ESP_OK或错误，超时禁止删除通道。
 */
esp_err_t CameraAudioRecordStop(CameraAudioStats *stats)
{
    if ((NULL == stats) || !s_running || s_playing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stop = true;
    xSemaphoreGive(s_lock);

    if (pdTRUE != xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)))
    {
        return ESP_ERR_TIMEOUT;
    }

    s_running = false;
    DeleteChannel();
    s_adc->enable(s_adc, false);
    *stats = s_stats;
    stats->overflows += s_dma_overflows;
    ESP_LOGI(kTag, "PCM samples=%lu peak=%lu square_sum=%llu overflows=%lu",
             (unsigned long)stats->samples, (unsigned long)stats->peak,
             (unsigned long long)stats->square_sum,
             (unsigned long)stats->overflows);
    ESP_LOGI(
        kTag, "TDM slot_peaks=%lu,%lu,%lu,%lu slot_squares=%llu,%llu,%llu,%llu",
        (unsigned long)stats->slot_peak[0], (unsigned long)stats->slot_peak[1],
        (unsigned long)stats->slot_peak[2], (unsigned long)stats->slot_peak[3],
        (unsigned long long)stats->slot_square_sum[0],
        (unsigned long long)stats->slot_square_sum[1],
        (unsigned long long)stats->slot_square_sum[2],
        (unsigned long long)stats->slot_square_sum[3]);
    return (s_failed || (stats->overflows != 0)) ? ESP_FAIL : ESP_OK;
}

/**
 * @authors Codex
 * @brief 复制已完成样本，环形缓冲跨末尾时仍保持PCM顺序。
 * @param output 非空可写目标。
 * @param capacity 字节容量，取偶数以保持16位对齐。
 * @return 实际复制字节数。
 */
size_t CameraAudioRead(uint8_t *output, size_t capacity)
{
    if ((NULL == output) || (NULL == s_lock))
    {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t samples = capacity / 2;
    samples = (samples > s_count) ? s_count : samples;

    for (size_t sample = 0; sample < samples; ++sample)
    {
        int16_t value = s_ring[s_tail];
        output[sample * 2] = (uint8_t)value;
        output[(sample * 2) + 1] = (uint8_t)((uint16_t)value >> 8);
        s_tail = (s_tail + 1) % AUDIO_RING_SAMPLES;
    }

    s_count -= samples;
    xSemaphoreGive(s_lock);
    return samples * 2;
}

/**
 * @authors Codex
 * @brief 读取同步错误状态；DMA计数只在ISR写入。
 * @return true表示音频不完整。
 */
bool CameraAudioFailed(void)
{
    if (NULL == s_lock)
    {
        return true;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool failed = s_failed || (s_dma_overflows != 0);
    xSemaphoreGive(s_lock);
    return failed;
}

/**
 * @authors Codex
 * @brief 创建标准I2S TX，以独立任务持续播放单声道样本。
 * @details
 * 标准I2S输出与录音TDM输入按模式分别创建。
 * 播放帧率固定24kHz，不能根据视频帧率调整音频采样率。
 * ES8311采用主机提供的MCLK，左右声道各16位。
 * 功放只在codec和通道初始化成功后开启。
 * 音量设置为-12dB，限制默认回放电平。
 * 任务启动前保存PCM指针和样本总数，期间禁止释放。
 * 所有失败分支关闭功放并删除创建的通道。
 * 播放错误不会修改PCM或媒体文件。
 * 播放任务与摄像头取帧任务不共享帧指针。
 * 主任务仍负责等待播放结束并关闭文件。
 * @param pcm 非空样本缓冲，直到停止完成前禁止释放。
 * @param samples 样本数量，不能为零。
 * @return ESP_OK或错误；错误时关闭功放和通道。
 */
esp_err_t CameraAudioPlaybackStart(const int16_t *pcm, size_t samples)
{
    if ((NULL == s_dac) || (NULL == pcm) || (samples == 0) || s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = AUDIO_BLOCK_SAMPLES;
    esp_err_t result = i2s_new_channel(&channel, &s_channel, NULL);

    if (ESP_OK != result)
    {
        return result;
    }

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = 21, .bclk = 38, .ws = 39, .dout = 40, .din = -1},
    };
    result = i2s_channel_init_std_mode(s_channel, &config);
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 2,
    };

    if ((ESP_OK == result)
        && ((s_dac->set_fs(s_dac, &format) != 0)
            || (s_dac->enable(s_dac, true) != 0)
            || (s_dac->set_vol(s_dac, -12.0f) != 0)))
    {
        result = ESP_FAIL;
    }

    if (ESP_OK == result)
    {
        result = i2s_channel_enable(s_channel);
    }

    if (ESP_OK == result)
    {
        result = BoardSetAmplifier(true);
    }

    s_stop = false;
    s_failed = false;
    s_playing = true;
    s_play_pcm = pcm;
    s_play_samples = samples;
    xSemaphoreTake(s_done, 0);

    if ((ESP_OK == result)
        && (pdPASS
            != xTaskCreate(PlaybackTask, "camera_speaker", 4096, NULL, 8,
                           NULL)))
    {
        result = ESP_ERR_NO_MEM;
    }

    if (ESP_OK != result)
    {
        DeleteChannel();
        BoardSetAmplifier(false);
        s_dac->enable(s_dac, false);
        s_play_pcm = NULL;
        return result;
    }

    s_running = true;
    ESP_LOGI(kTag, "PLAY_PCM samples=%u", (unsigned)samples);
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 停止播放且等待任务放弃PCM指针，再允许浏览器释放缓冲。
 * @details
 * 已播放完毕的任务仍释放完成信号，停止接口同样等待确认。
 * 只有停止超时禁止释放PCM，普通播放失败已不持有该指针。
 * 关闭功放在主任务完成，不在音频任务中改写扩展器输出。
 * 删除TX后清空PCM引用，防止后续查询访问旧数据。
 * 此接口不释放浏览器提供的PCM，所有权始终保留在调用者。
 * 没有正在播放时返回成功，允许幂等关闭浏览器。
 * @return ESP_OK或超时；已停止时返回成功。
 */
esp_err_t CameraAudioPlaybackStop(void)
{
    if (!s_running || !s_playing)
    {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stop = true;
    xSemaphoreGive(s_lock);

    if (pdTRUE != xSemaphoreTake(s_done, pdMS_TO_TICKS(1000)))
    {
        return ESP_ERR_TIMEOUT;
    }

    s_running = false;
    DeleteChannel();
    BoardSetAmplifier(false);
    s_dac->enable(s_dac, false);
    s_play_pcm = NULL;
    return s_failed ? ESP_FAIL : ESP_OK;
}
