/**
 * @file media_list.h
 * @author Codex
 * @brief 供LVGL菜单使用的四行缩略图列表模型，不操作文件或LCD。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：移除传统绘制接口，模型由LVGL使用；法律信息：None。
 */
#ifndef __MEDIA_LIST_H_
#define __MEDIA_LIST_H_
#include "storage/media_browser.h"
#include "storage/media_thumbnail.h"

/* 屏幕为320x240像素，RGB565大端，渲染图可直接提交给LCD。 */
#define MEDIA_LIST_WIDTH  (320)
#define MEDIA_LIST_HEIGHT (240)
#define MEDIA_LIST_BYTES  (MEDIA_LIST_WIDTH * MEDIA_LIST_HEIGHT * 2)

typedef struct
{
    MediaBrowserPage page;
    const uint8_t *thumbnails[MEDIA_BROWSER_ROWS]; /* 借用48x36图像。 */
    bool thumbnail_valid[MEDIA_BROWSER_ROWS];      /* 无效预览用类型占位图。 */
    const char *notice; /* 借用非空提示字符串，不长期持有。 */
} MediaListView;

#endif
