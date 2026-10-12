/**
 * @file camera_audio.h
 * @author Codex
 * @brief ES7210单声道录音和ES8311回放，PCM为24kHz/16bit。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增独立音频任务；法律信息：None。
 */
#ifndef __CAMERA_AUDIO_H_
#define __CAMERA_AUDIO_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* PCM单位：每秒24000个有符号16位单声道采样，48000字节/秒。 */
#define AUDIO_SAMPLE_RATE  (24000)
#define AUDIO_SAMPLE_BYTES (2)

typedef struct
{
    uint32_t samples;            /* 实际已采集的单声道样本数，不含丢弃数据。 */
    uint32_t overflows;          /* DMA队列或环形缓冲溢出次数，非零阻止发布。 */
    uint32_t peak;               /* 绝对峰值，满量程32768。 */
    uint32_t slot_peak[4];       /* 四个TDM槽峰值，供物理麦克风通道诊断。 */
    uint64_t slot_square_sum[4]; /* 各槽平方和，不写入AVI音轨。 */
    uint64_t square_sum;         /* 样本平方和，用于主机计算RMS。 */
} CameraAudioStats;

/**
 * @authors Codex
 * @brief 在共享I2C上创建编解码器；分配4秒PSRAM录音缓冲。
 * @param bus 有效共享总线，不转移所有权。
 * @return ESP_OK或初始化错误，不自动降级为无声录像。
 */
esp_err_t CameraAudioInit(i2c_master_bus_handle_t bus);
/**
 * @authors Codex
 * @brief 启动ES7210 TDM四槽采集，保存MIC1槽为单声道。
 * @return ESP_OK或错误，仅主任务调用。
 */
esp_err_t CameraAudioRecordStart(void);
/**
 * @authors Codex
 * @brief 等待音频任务退出，再关闭RX；保留环形缓冲供排空。
 * @param stats 非空统计输出，返回后不再被任务更新。
 * @return ESP_OK或停止超时；超时不删除正在使用的资源。
 */
esp_err_t CameraAudioRecordStop(CameraAudioStats *stats);
/**
 * @authors Codex
 * @brief 同步排空部分PCM数据，保持样本边界。
 * @param output 非空目标缓冲，由调用者持有。
 * @param capacity 缓冲字节数，至少2字节。
 * @return 实际复制字节数；环形缓冲由本模块持有。
 */
size_t CameraAudioRead(uint8_t *output, size_t capacity);
/**
 * @authors Codex
 * @brief 查询采集溢出或驱动错误。
 * @return true表示本次音频已丢失，应停止且丢弃录像。
 */
bool CameraAudioFailed(void);
/**
 * @authors Codex
 * @brief 独立任务播放已加载的PCM，打开功放。
 * @param pcm 非空有符号16位数据，直到PlaybackStop返回前保持有效。
 * @param samples 样本数，单位个，不能为零。
 * @return ESP_OK或错误；不转移PCM所有权。
 */
esp_err_t CameraAudioPlaybackStart(const int16_t *pcm, size_t samples);
/**
 * @authors Codex
 * @brief 等待播放任务停止，然后关闭TX和功放。
 * @return ESP_OK或停止超时；超时调用者不得释放PCM。
 */
esp_err_t CameraAudioPlaybackStop(void);
#endif
