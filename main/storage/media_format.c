/**
 * @file media_format.c
 * @author Codex
 * @brief RGB565 图片转换和 RIFF AVI 单视频流封装。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：新增格式实现；法律信息：None，自行实现文件字段。
 */
#include "media_format.h"
#include <string.h>

/**
 * @authors Codex
 * @brief 按小端写值，避免结构体填充和主机字节序影响文件。
 * @param output 非空四字节目标。
 * @param value 要保存的值。
 * @return None
 */
void MediaPut32(uint8_t *output, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
    {
        output[i] = (uint8_t)(value >> (8 * i));
    }
}

/**
 * @authors Codex
 * @brief 写入 BITMAPINFOHEADER，BMP 与 AVI 共用相同像素布局。
 * @param output 非空 40 字节目标。
 * @return None
 */
static void BitmapInfo(uint8_t *output)
{
    /* BI_RGB 为零；24 位 BGR、单平面、正高度用于底部优先的行顺序。 */
    MediaPut32(output, 40);
    MediaPut32(output + 4, MEDIA_WIDTH);
    MediaPut32(output + 8, MEDIA_HEIGHT);
    output[12] = 1;
    output[14] = 24;
    MediaPut32(output + 20, MEDIA_FRAME_BYTES);
}

/**
 * @authors Codex
 * @brief 构造 BMP 文件头和像素偏移，不包含像素数据。
 * @param header 非空 54 字节目标。
 * @return None
 */
void MediaBmpHeader(uint8_t *header)
{
    memset(header, 0, 54);
    memcpy(header, "BM", 2);
    MediaPut32(header + 2, 54 + MEDIA_FRAME_BYTES);
    MediaPut32(header + 10, 54);
    BitmapInfo(header + 14);
}

/**
 * @authors Codex
 * @brief 构造一个 BI_RGB 视频流；长度含 movi 块及 idx1 索引。
 * @param header 非空 224 字节目标。
 * @param frames 完整帧数。
 * @param period_us 实测平均帧周期，微秒。
 * @return None
 */
void MediaAviHeader(uint8_t *header, uint32_t frames, uint32_t period_us)
{
    memset(header, 0, MEDIA_AVI_HEADER_BYTES);
    /* hdrl 从 12 开始，strl 从 88 开始，movi 从 212 开始。
     * RIFF 大小不含最前面的八字节，idx1 数据每帧占 16 字节。
     * 帧块是 00db，每块八字节头和固定大小像素。
     * 最大录像帧数在应用限制，所有长度都在 32 位范围。
     * AVI_HASINDEX 仅最终回填时设置，初始头不宣称已有索引。 */
    memcpy(header, "RIFF", 4);
    MediaPut32(header + 4, 224 + (frames * (MEDIA_FRAME_BYTES + 24)));
    memcpy(header + 8, "AVI LIST", 8);
    MediaPut32(header + 16, 192);
    memcpy(header + 20, "hdrlavih", 8);
    MediaPut32(header + 28, 56);
    MediaPut32(header + 32, period_us);
    MediaPut32(header + 36,
               (uint32_t)(((uint64_t)MEDIA_FRAME_BYTES * 1000000) / period_us));
    MediaPut32(header + 44, frames ? 0x10 : 0);
    MediaPut32(header + 48, frames);
    MediaPut32(header + 56, 1);
    MediaPut32(header + 60, MEDIA_FRAME_BYTES);
    MediaPut32(header + 64, MEDIA_WIDTH);
    MediaPut32(header + 68, MEDIA_HEIGHT);
    memcpy(header + 88, "LIST", 4);
    MediaPut32(header + 92, 116);
    memcpy(header + 96, "strlstrh", 8);
    MediaPut32(header + 104, 56);
    memcpy(header + 108, "vidsDIB ", 8);
    /* scale/rate 用微秒整数表示帧周期；播放器不必假定恒定 5 FPS。
     * dwLength 是总帧数，dwSampleSize=0 表示按块边界取样。
     * rcFrame 使用 16 位宽高，固定 QVGA 不会溢出。
     * 音频流未创建，录像文件只包含图像。 */
    MediaPut32(header + 128, period_us);
    MediaPut32(header + 132, 1000000);
    MediaPut32(header + 140, frames);
    MediaPut32(header + 144, MEDIA_FRAME_BYTES);
    MediaPut32(header + 148, UINT32_MAX);
    header[160] = (uint8_t)MEDIA_WIDTH;
    header[161] = (uint8_t)(MEDIA_WIDTH >> 8);
    header[162] = (uint8_t)MEDIA_HEIGHT;
    memcpy(header + 164, "strf", 4);
    MediaPut32(header + 168, 40);
    BitmapInfo(header + 172);
    memcpy(header + 212, "LIST", 4);
    MediaPut32(header + 216, 4 + (frames * (MEDIA_FRAME_BYTES + 8)));
    memcpy(header + 220, "movi", 4);
}

/**
 * @authors Codex
 * @brief 扩展 RGB565 各颜色位宽，逐像素反转，不修改采集帧。
 * @param source 非空大端 RGB565 输入行。
 * @param output 非空 BGR24 输出行。
 * @return None
 */
void MediaConvertRowWidth(const uint8_t *source, uint8_t *output,
                          unsigned width)
{
    /* 取反后的 x 与 LCD 使用相同映射；复制完整像素避免交换字节。
     * 低位补齐采用位复制，将 5/6 位颜色映射到完整 8 位范围。
     * 输出先蓝后绿后红，符合 Windows 位图的 BGR 排列。
     * 行的上下顺序由写文件调用者控制，不在此处隐式翻转。 */
    for (unsigned x = 0; x < width; ++x)
    {
        unsigned offset = (width - 1 - x) * 2;
        uint16_t pixel = ((uint16_t)source[offset] << 8) | source[offset + 1];
        uint8_t blue = pixel & 31;
        uint8_t green = (pixel >> 5) & 63;
        uint8_t red = (pixel >> 11) & 31;
        output[x * 3] = (blue << 3) | (blue >> 2);
        output[(x * 3) + 1] = (green << 2) | (green >> 4);
        output[(x * 3) + 2] = (red << 3) | (red >> 2);
    }
}

/**
 * @authors Codex
 * @brief 为AVI添加PCM音频流，按实际movi和索引长度填写RIFF。
 * @param header 非空324字节目标，不与输入重叠。
 * @param frames 完整视频帧数。
 * @param period_us 非零播放帧周期，微秒。
 * @param audio_bytes 完整PCM字节数，16位样本需偶数。
 * @param movi_bytes 所有音视频块总字节数，含每块8字节头。
 * @param chunks 完整音视频块总数，每个索引16字节。
 * @return None。
 */
void MediaAvHeader(uint8_t *header, uint32_t frames, uint32_t period_us,
                   uint32_t audio_bytes, uint32_t movi_bytes, uint32_t chunks)
{
    /* 最新修改：复用原视频strl，新增100字节PCM strl。
     * 视频流仍为00db，音频流01wb，索引依据实际偏移生成。
     * 不通过结构体序列化，避免编译器对齐改变WAVEFORMAT。
     * 24kHz单声道16位PCM每秒48000字节，block align=2。
     * 总视频时长由PCM样本数分配，避免写卡延迟使音轨变速。
     * PCM无压缩，不依赖运行时音频编码器。 */
    memset(header, 0, MEDIA_AV_HEADER_BYTES);
    MediaAviHeader(header, frames, period_us);
    MediaPut32(header + 4, MEDIA_AV_HEADER_BYTES + movi_bytes + (chunks * 16));
    MediaPut32(header + 16, 292);
    MediaPut32(header + 36,
               (uint32_t)(((uint64_t)MEDIA_FRAME_BYTES * 1000000) / period_us)
                   + 48000);
    MediaPut32(header + 56, 2);
    memcpy(header + 212, "LIST", 4);
    MediaPut32(header + 216, 92);
    memcpy(header + 220, "strlstrh", 8);
    MediaPut32(header + 228, 56);
    memcpy(header + 232, "auds", 4);
    MediaPut32(header + 252, 2);
    MediaPut32(header + 256, 48000);
    MediaPut32(header + 264, audio_bytes / 2);
    MediaPut32(header + 268, 15360);
    MediaPut32(header + 272, UINT32_MAX);
    MediaPut32(header + 276, 2);
    memcpy(header + 288, "strf", 4);
    MediaPut32(header + 292, 16);
    header[296] = 1; /* WAVE_FORMAT_PCM。 */
    header[298] = 1; /* 单声道。 */
    MediaPut32(header + 300, 24000);
    MediaPut32(header + 304, 48000);
    header[308] = 2;  /* 16bit单声道块对齐字节数。 */
    header[310] = 16; /* 每采样16位。 */
    memcpy(header + 312, "LIST", 4);
    MediaPut32(header + 316, 4 + movi_bytes);
    memcpy(header + 320, "movi", 4);
}

/**
 * @authors Codex
 * @brief 保留录像的 QVGA 行转换接口。
 * @param source 非空 640 字节输入行。
 * @param output 非空 960 字节输出行。
 * @return None。
 */
void MediaConvertRow(const uint8_t *source, uint8_t *output)
{
    MediaConvertRowWidth(source, output, MEDIA_WIDTH);
}
