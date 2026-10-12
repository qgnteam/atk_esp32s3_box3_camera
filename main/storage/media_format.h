/**
 * @file media_format.h
 * @author Codex
 * @brief BMP 和无压缩 AVI 文件格式，独立于硬件以便验证。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：新增图片与视频封装；法律信息：None。
 */
#ifndef __MEDIA_FORMAT_H_
#define __MEDIA_FORMAT_H_
#include <stddef.h>
#include <stdint.h>

/* QVGA BGR24 每行正好四字节对齐；文件采用小端字段。 */
#define MEDIA_WIDTH            (320)
#define MEDIA_HEIGHT           (240)
#define MEDIA_FRAME_BYTES      (MEDIA_WIDTH * MEDIA_HEIGHT * 3)
#define MEDIA_AVI_HEADER_BYTES (224)
#define MEDIA_AV_HEADER_BYTES  (324)

/**
 * @authors Codex
 * @brief 生成 54 字节 BMP 头，正高度表示从下往上存储。
 * @param header 非空且至少 54 字节的输出。
 * @return None
 */
void MediaBmpHeader(uint8_t *header);
/**
 * @authors Codex
 * @brief 生成 AVI 头；帧数和实际平均帧周期在停止录像后回填。
 * @param header 非空且至少 MEDIA_AVI_HEADER_BYTES 字节。
 * @param frames 完整帧数，索引每帧 16 字节。
 * @param period_us 每帧平均微秒数，必须大于零。
 * @return None
 */
void MediaAviHeader(uint8_t *header, uint32_t frames, uint32_t period_us);
/**
 * @authors Codex
 * @brief 将大端 RGB565 一行转为 BGR24 并水平反转，与 LCD 一致。
 * @param source 至少 640 字节的有效输入，不转移所有权。
 * @param output 至少 960 字节的输出，不允许重叠。
 * @return None
 */
void MediaConvertRow(const uint8_t *source, uint8_t *output);
/**
 * @authors Codex
 * @brief 写入四字节小端数值，用于块长度和索引。
 * @param output 至少四字节的输出。
 * @param value 数值。
 * @return None
 */
void MediaPut32(uint8_t *output, uint32_t value);
/**
 * @authors Codex
 * @brief 生成双流AVI头，视频BGR24和24kHz/16bit单声道PCM。
 * @param header 非空324字节输出。
 * @param frames 视频帧数。
 * @param period_us 恒定播放帧周期，单位微秒且非零。
 * @param audio_bytes PCM数据字节数，必须为偶数。
 * @param movi_bytes movi实际块字节数，不含LIST头和类型标识。
 * @param chunks idx1条目数，包含视频块和音频块。
 * @return None；各长度由写入者检查32位范围。
 */
void MediaAvHeader(uint8_t *header, uint32_t frames, uint32_t period_us,
                   uint32_t audio_bytes, uint32_t movi_bytes, uint32_t chunks);
#endif
