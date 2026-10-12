/**
 * @file media_thumbnail.h
 * @author Codex
 * @brief BMP及项目AVI首帧的48x36 RGB565缩略图读取接口。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增只读预览；法律信息：None。
 */
#ifndef __MEDIA_THUMBNAIL_H_
#define __MEDIA_THUMBNAIL_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 四比三预览尺寸，像素为大端RGB565，与LCD数据格式一致。 */
#define MEDIA_THUMB_WIDTH      (48)
#define MEDIA_THUMB_HEIGHT     (36)
#define MEDIA_THUMB_BYTES      (MEDIA_THUMB_WIDTH * MEDIA_THUMB_HEIGHT * 2)
#define MEDIA_THUMB_WORK_BYTES (960)

/**
 * @authors Codex
 * @brief 只读打开图片或AVI首帧，按最近邻采样生成预览。
 * @param path 非空固定媒体目录文件路径，不改写文件。
 * @param output 非空至少MEDIA_THUMB_BYTES字节，失败保持全零。
 * @param work 非空至少MEDIA_THUMB_WORK_BYTES字节临时行缓冲。
 * @param work_bytes 行缓冲容量，字节，不转移内存所有权。
 * @return true表示完整有效预览；损坏/不支持文件返回false。
 * @note 仅支持项目320x240 BGR24格式，不开启音频或创建回放任务。
 */
bool MediaThumbnailRead(const char *path, uint8_t *output, uint8_t *work,
                        size_t work_bytes);
#endif
