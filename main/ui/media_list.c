/**
 * @file media_list.c
 * @author Codex
 * @brief 可滚动媒体列表，四行48x36缩略图和文件名高亮。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：文件列表替代单文件名称页；法律信息：None。
 */
#include "media_list.h"
#include <stdio.h>
#include <string.h>
#include "display/lcd_font.h"

/* 顶部28像素，四行各44像素，底部36像素用于按钮提示。
 * 缩略图保留4:3比例，文件名最大可见20字符，不与预览重叠。
 * 高亮背景蓝色、边线青色，其余行使用交替深色背景。 */
#define LIST_TOP        (28)
#define LIST_ROW_HEIGHT (44)
#define THUMB_LEFT      (8)
#define THUMB_TOP       (4)
#define NAME_LEFT       (64)
static const uint16_t kBackground = 0x0841;
static const uint16_t kSelected = 0x1a8d;
static const uint16_t kAccent = 0x07ff;
static const uint16_t kText = 0xffff;
static const uint16_t kMuted = 0x9cf3;

/**
 * @authors Codex
 * @brief 填充屏幕内部矩形，保持RGB565大端颜色格式。
 * @param pixels 非空完整320x240输出。
 * @param x 左边界，像素，非负且矩形在屏幕内。
 * @param y 上边界，像素，非负且矩形在屏幕内。
 * @param width 宽度，像素。
 * @param height 高度，像素。
 * @param color 主机RGB565颜色。
 * @return None，坐标由本模块固定布局保证。
 */
static void Rectangle(uint8_t *pixels, unsigned x, unsigned y, unsigned width,
                      unsigned height, uint16_t color)
{
    for (unsigned row = y; row < y + height; ++row)
    {
        for (unsigned column = x; column < x + width; ++column)
        {
            unsigned offset = (row * MEDIA_LIST_WIDTH + column) * 2;
            pixels[offset] = color >> 8;
            pixels[offset + 1] = (uint8_t)color;
        }
    }
}

/**
 * @authors Codex
 * @brief 绘制一行名称、类型及缩略图，不改变预览内容。
 * @details 有效预览直接按行拷贝，方向已在缩略图读取时校正。
 * AVI行显示VIDEO，图片显示PHOTO，损坏文件显示NO PREVIEW。
 * 缺失预览用带边框的类型文字占位，不沿用上一页的旧图。
 * 选中行青色边线与蓝色背景保持明显区分。
 * 长名称显示前19字符及省略号，完整名称仍保留用于打开。
 * 每行开始结束坐标固定，最后一行不会侵入底部操作区。
 * @param view 非空且行数有效的只读模型。
 * @param pixels 非空完整输出。
 * @param row 本页行号，范围0至3。
 * @return None，不申请内存或访问TF。
 */
static void RenderRow(const MediaListView *view, uint8_t *pixels, unsigned row)
{
    unsigned y = LIST_TOP + row * LIST_ROW_HEIGHT;
    bool selected = (row == view->page.selected_row);
    Rectangle(pixels, 4, y, MEDIA_LIST_WIDTH - 8, LIST_ROW_HEIGHT - 2,
              selected ? kSelected : ((row & 1) ? 0x1082 : kBackground));

    if (selected)
    {
        Rectangle(pixels, 4, y, 2, LIST_ROW_HEIGHT - 2, kAccent);
    }

    unsigned thumb_y = y + THUMB_TOP;
    Rectangle(pixels, THUMB_LEFT - 1, thumb_y - 1, MEDIA_THUMB_WIDTH + 2,
              MEDIA_THUMB_HEIGHT + 2, selected ? kAccent : kMuted);
    Rectangle(pixels, THUMB_LEFT, thumb_y, MEDIA_THUMB_WIDTH,
              MEDIA_THUMB_HEIGHT, 0x0000);
    const char *name = view->page.names[row];
    size_t length = strnlen(name, MEDIA_BROWSER_NAME_BYTES);
    bool video = (length >= 4) && (memcmp(name + length - 4, ".AVI", 4) == 0);

    if (view->thumbnail_valid[row] && (NULL != view->thumbnails[row]))
    {
        for (unsigned line = 0; line < MEDIA_THUMB_HEIGHT; ++line)
        {
            unsigned offset =
                ((thumb_y + line) * MEDIA_LIST_WIDTH + THUMB_LEFT) * 2;
            memcpy(pixels + offset,
                   view->thumbnails[row] + line * MEDIA_THUMB_WIDTH * 2,
                   MEDIA_THUMB_WIDTH * 2);
        }
    }
    else
    {
        LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, THUMB_LEFT + 6,
                    thumb_y + 10, video ? "VIDEO" : "PHOTO", 1, kMuted);
        LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, THUMB_LEFT + 6,
                    thumb_y + 22, "--", 1, kMuted);
    }

    char label[24] = {0};
    snprintf(label, sizeof(label), length > 20 ? "%.19s." : "%.20s", name);
    LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, NAME_LEFT, y + 4,
                label, 2, kText);
    char detail[40] = {0};
    snprintf(detail, sizeof(detail), "%s%s", video ? "VIDEO" : "PHOTO",
             view->thumbnail_valid[row] ? "" : " - NO PREVIEW");
    LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, NAME_LEFT, y + 26,
                detail, 1, kMuted);
}

/**
 * @authors Codex
 * @brief 输出完整菜单图像，供LCD显示和主机使用同一绘制代码验证。
 * @details 模型没有文件句柄，绘制过程不能阻塞卡读取或改变音频状态。
 * 背景与未使用的末页区域先清空，避免上一页残影。
 * 列表标题显示选中项位置/总数，不将页内行号误称为文件总编号。
 * 页面最多四个缩略图，输入行数异常时不访问数组。
 * 空目录仍显示操作提示，并在内容区显示无文件或卡错误。
 * 操作区提示K1上移、K0下移、K2打开、长按K2取消。
 * 输出为RGB565大端，LCD不再执行水平镜像。
 * @param view 非空页模型，缩略图只借用至返回。
 * @param pixels 非空完整320x240缓冲，写入前容量由调用者保证。
 * @return None，所有绘制都在调用者任务中同步完成。
 */
void MediaListRender(const MediaListView *view, uint8_t *pixels)
{
    if (NULL == pixels)
    {
        return;
    }

    Rectangle(pixels, 0, 0, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, kBackground);

    if ((NULL == view) || (view->page.count > MEDIA_BROWSER_ROWS)
        || ((view->page.count > 0)
            && (view->page.selected_row >= view->page.count)))
    {
        return;
    }

    char title[32] = {0};
    unsigned current =
        (view->page.count > 0) ? view->page.selected_index + 1 : 0;
    snprintf(title, sizeof(title), "MEDIA %u/%u", current,
             view->page.file_count);
    LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, 6, 6, title, 2,
                kText);
    Rectangle(pixels, 4, 24, MEDIA_LIST_WIDTH - 8, 1, kAccent);

    for (unsigned row = 0; row < view->page.count; ++row)
    {
        RenderRow(view, pixels, row);
    }

    if (view->page.count == 0)
    {
        LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, 12, 92,
                    "NO PHOTOS OR VIDEOS", 2, kMuted);
        LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, 12, 120,
                    (NULL != view->notice) ? view->notice : "", 1, kMuted);
    }
    else if ((NULL != view->notice) && (view->notice[0] != '\0'))
    {
        LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, 8, 205,
                    view->notice, 1, kMuted);
    }

    LcdFontDraw(pixels, MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT, 8, 216,
                "K0: DOWN  K1: UP  K2: OPEN  HOLD K2: CANCEL", 1, kText);
}
