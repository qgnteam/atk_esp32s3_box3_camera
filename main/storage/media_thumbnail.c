/**
 * @file media_thumbnail.c
 * @author Codex
 * @brief 有界解析首帧并按行抽样，避免为列表加载整段录像或音轨。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：照片及录像缩略图；法律信息：None。
 */
#include "media_thumbnail.h"
#include <stdio.h>
#include <string.h>
#include "media_format.h"

/* 项目录像小于100MiB；有限块扫描避免损坏文件造成无界循环。 */
#define THUMB_MAX_FILE_BYTES (100U * 1024U * 1024U)
#define THUMB_MAX_CHUNKS     (4096)

/**
 * @authors Codex
 * @brief 从字节数组读取小端字段，避免未对齐结构体访问。
 * @param bytes 非空且至少四字节输入。
 * @return 32位长度或偏移。
 */
static uint32_t Get32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
           | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * @authors Codex
 * @brief 验证固定格式并定位BMP像素或AVI首个00db块。
 * @details AVI先检查实际文件大小，再在movi范围内跳过PCM块。
 * 首帧读取无需解析或播放音轨，因此不会申请整段音频内存。
 * 每块长度先由剩余文件范围约束，再计算下一个偏移。
 * PCM块必须为偶数字节，拒绝未对齐的16位采样，不猜测填充。
 * 不使用idx1指针定位首帧，避免畸形索引指向文件之外。
 * 首帧尺寸、位深、BI_RGB及正高度与完整回放一致。
 * @param file 非空只读文件，返回时文件仍由调用者关闭。
 * @param file_bytes 实际文件长度，字节。
 * @param pixels 非空首帧像素偏移输出。
 * @return 是否找到完整可用的320x240 BGR24首帧。
 */
static bool FindPixels(FILE *file, uint32_t file_bytes, uint32_t *pixels)
{
    uint8_t header[MEDIA_AV_HEADER_BYTES] = {0};

    if (fread(header, 1, sizeof(header), file) != sizeof(header))
    {
        return false;
    }

    if ((header[0] == 'B') && (header[1] == 'M'))
    {
        bool valid =
            (file_bytes == 54 + MEDIA_FRAME_BYTES)
            && (Get32(header + 2) == file_bytes) && (Get32(header + 10) == 54)
            && (Get32(header + 14) == 40) && (Get32(header + 18) == MEDIA_WIDTH)
            && (Get32(header + 22) == MEDIA_HEIGHT) && (header[26] == 1)
            && (header[27] == 0) && (header[28] == 24) && (header[29] == 0)
            && (Get32(header + 30) == 0);
        *pixels = 54;
        return valid;
    }

    uint32_t streams = Get32(header + 56);
    uint32_t start =
        (streams == 2) ? MEDIA_AV_HEADER_BYTES : MEDIA_AVI_HEADER_BYTES;

    /* 原单视频和新版音视频AVI均支持，其他流数不猜测其布局。
     * 文件必须包含完整movi标识和标准DIB视频格式。
     * 音频块可以位于首帧之前，按实际长度跳过而不读音轨。 */
    if ((memcmp(header, "RIFF", 4) != 0) || (memcmp(header + 8, "AVI ", 4) != 0)
        || ((streams != 1) && (streams != 2))
        || (Get32(header + 4) != file_bytes - 8) || (Get32(header + 48) == 0)
        || (memcmp(header + 108, "vidsDIB ", 8) != 0)
        || (Get32(header + 176) != MEDIA_WIDTH)
        || (Get32(header + 180) != MEDIA_HEIGHT) || (header[184] != 1)
        || (header[185] != 0) || (header[186] != 24) || (header[187] != 0)
        || (Get32(header + 188) != 0)
        || (memcmp(header + start - 12, "LIST", 4) != 0)
        || (memcmp(header + start - 4, "movi", 4) != 0))
    {
        return false;
    }

    uint32_t length = Get32(header + start - 8);

    if ((length < 4) || (length - 4 > file_bytes - start))
    {
        return false;
    }

    uint32_t position = start;
    uint32_t end = start + length - 4;

    for (unsigned chunk = 0; (chunk < THUMB_MAX_CHUNKS) && (position < end);
         ++chunk)
    {
        uint8_t block[8] = {0};

        if ((end - position < sizeof(block))
            || (fseek(file, position, SEEK_SET) != 0)
            || (fread(block, 1, sizeof(block), file) != sizeof(block)))
        {
            return false;
        }

        uint32_t bytes = Get32(block + 4);

        if (bytes > end - position - 8)
        {
            return false;
        }

        if (memcmp(block, "00db", 4) == 0)
        {
            *pixels = position + 8;
            return bytes == MEDIA_FRAME_BYTES;
        }

        if ((streams != 2) || (memcmp(block, "01wb", 4) != 0)
            || ((bytes & 1) != 0))
        {
            return false;
        }

        position += 8 + bytes;
    }

    return false;
}

/**
 * @authors Codex
 * @brief 抽取36个源扫描行，每行选48个像素，输出标准方向预览。
 * @details 保存文件已纠正左右方向，此处只反转底部优先扫描行。
 * 使用目标像素中心对应的最近邻位置，边缘采样不超出QVGA。
 * 每次读取960字节源行，工作缓冲由存储模块复用。
 * 缩略图总3456字节，列表四行合计13824字节。
 * 所有错误路径关闭文件，部分图像不会作为有效预览显示。
 * 临时行缓冲与输出不得重叠，调用者保证各自容量。
 * @param path 非空媒体路径，只读打开。
 * @param output 非空完整预览输出，失败清零。
 * @param work 非空至少960字节源行缓冲。
 * @param work_bytes 行缓冲容量，字节。
 * @return 是否完整读取并转换成功。
 */
bool MediaThumbnailRead(const char *path, uint8_t *output, uint8_t *work,
                        size_t work_bytes)
{
    if (NULL == output)
    {
        return false;
    }

    memset(output, 0, MEDIA_THUMB_BYTES);

    if ((NULL == path) || (NULL == work)
        || (work_bytes < MEDIA_THUMB_WORK_BYTES))
    {
        return false;
    }

    FILE *file = fopen(path, "rb");

    if (NULL == file)
    {
        return false;
    }

    bool valid = (fseek(file, 0, SEEK_END) == 0);
    long size = valid ? ftell(file) : -1;
    valid = valid && (size >= MEDIA_AV_HEADER_BYTES)
            && (size <= THUMB_MAX_FILE_BYTES)
            && (fseek(file, 0, SEEK_SET) == 0);
    uint32_t pixels = 0;
    valid = valid && FindPixels(file, (uint32_t)size, &pixels);

    for (unsigned row = 0; valid && (row < MEDIA_THUMB_HEIGHT); ++row)
    {
        unsigned source_y =
            ((row * 2 + 1) * MEDIA_HEIGHT) / (MEDIA_THUMB_HEIGHT * 2);
        uint32_t offset =
            pixels + (MEDIA_HEIGHT - 1 - source_y) * MEDIA_THUMB_WORK_BYTES;
        valid = (fseek(file, offset, SEEK_SET) == 0)
                && (fread(work, 1, MEDIA_THUMB_WORK_BYTES, file)
                    == MEDIA_THUMB_WORK_BYTES);

        if (!valid)
        {
            break;
        }

        /* BGR24量化为RGB565大端；输出坐标与完整回放相同，不再次镜像。 */
        for (unsigned column = 0; column < MEDIA_THUMB_WIDTH; ++column)
        {
            unsigned source_x =
                ((column * 2 + 1) * MEDIA_WIDTH) / (MEDIA_THUMB_WIDTH * 2);
            const uint8_t *sample = work + source_x * 3;
            uint16_t color = ((uint16_t)(sample[2] >> 3) << 11)
                             | ((uint16_t)(sample[1] >> 2) << 5)
                             | (sample[0] >> 3);
            unsigned target = (row * MEDIA_THUMB_WIDTH + column) * 2;
            output[target] = color >> 8;
            output[target + 1] = (uint8_t)color;
        }
    }

    valid = valid && !ferror(file);

    if (fclose(file) != 0)
    {
        valid = false;
    }

    if (!valid)
    {
        memset(output, 0, MEDIA_THUMB_BYTES);
    }

    return valid;
}
