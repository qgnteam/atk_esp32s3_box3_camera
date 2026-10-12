/**
 * @file media_list.h
 * @author Codex
 * @brief 四行缩略图列表的纯RGB565绘制模型，不操作文件或LCD。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增列表界面；法律信息：None。
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

/**
 * @authors Codex
 * @brief 绘制四行文件列表、选中高亮、页序及操作提示。
 * @param view 非空模型，最多四行，每个名称终止于32字节内。
 * @param pixels 非空至少MEDIA_LIST_BYTES字节，调用者持有。
 * @return None，参数无效时保留清空背景，不读取越界缩略图。
 * @note 只读借用预览，渲染结束即可释放模型；不修改媒体文件。
 */
void MediaListRender(const MediaListView *view, uint8_t *pixels);
#endif
