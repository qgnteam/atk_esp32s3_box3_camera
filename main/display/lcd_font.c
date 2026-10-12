/**
 * @file lcd_font.c
 * @author Codex
 * @brief 原项目点阵字体的有界绘制，不依赖LCD驱动或外部字体。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：共用预览、列表和主机渲染；法律信息：None。
 */
#include "lcd_font.h"
#include <stddef.h>

/* 自行定义5x7点阵，显示2倍像素字形；只覆盖英文菜单和文件名。
 * 每行掩码低5位为从左到右像素，字形不依赖外部字体资源。
 * 标点与空格保证路径、模式、确认选项可读，未知字显示问号。
 * 字模和UI条缓冲仅由主任务访问，录音任务不绘制LCD。 */
typedef struct
{
    char character;
    uint8_t rows[7];
} LcdGlyph;
static const LcdGlyph kGlyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {':', {0, 12, 12, 0, 12, 12, 0}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'/', {1, 2, 2, 4, 8, 8, 16}},
    {'>', {16, 8, 4, 2, 4, 8, 16}},      {'<', {1, 2, 4, 8, 4, 2, 1}},
    {'?', {14, 17, 1, 2, 4, 0, 4}},      {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'[', {14, 8, 8, 8, 8, 8, 14}},      {']', {14, 2, 2, 2, 2, 2, 14}},
    {'_', {0, 0, 0, 0, 0, 0, 31}},       {'=', {0, 0, 31, 0, 31, 0, 0}},
    {'!', {4, 4, 4, 4, 4, 0, 4}},
};

/**
 * @authors Codex
 * @brief 在RGB565大端图像中绘制同一套菜单点阵。
 * @details 字模保留原项目字符定义，字体不会修改背景像素。
 * 每字间距为6*scale，字形为5*scale乘7*scale像素。
 * 使用有符号坐标裁剪，可跨LCD条带边界绘制同一文字行。
 * 最大图像限定320x240，索引乘积不会溢出无符号32位。
 * 未知字符绘制问号，小写文件名显示为大写。
 * 不申请内存，也不持有调用者字符串或输出图像。
 * @param pixels 非空RGB565大端输出，容量由调用者保证。
 * @param width 图像宽度，像素，最多320。
 * @param height 图像高度，像素，最多240。
 * @param x 起始横坐标，像素。
 * @param y 起始纵坐标，像素。
 * @param text 非空ASCII终止字符串。
 * @param scale 缩放倍率，1或2。
 * @param color RGB565颜色，按高字节先输出。
 * @return None；超出图像的像素不写入。
 */
void LcdFontDraw(uint8_t *pixels, unsigned width, unsigned height, int x, int y,
                 const char *text, unsigned scale, uint16_t color)
{
    if ((NULL == pixels) || (NULL == text) || (width == 0) || (width > 320)
        || (height == 0) || (height > 240) || (scale == 0) || (scale > 2)
        || (x < -320) || (x >= (int)width) || (y < -240) || (y >= (int)height))
    {
        return;
    }

    /* 字符数也有上限，拒绝无限扫描长文本，保留左侧裁剪空间。 */
    unsigned limit = (width + 320) / (6 * scale);

    for (unsigned index = 0; (index < limit) && (text[index] != '\0'); ++index)
    {
        char character = text[index];

        if ((character >= 'a') && (character <= 'z'))
        {
            character -= ('a' - 'A');
        }

        static const uint8_t kUnknownGlyph[7] = {14, 17, 1, 2, 4, 0, 4};
        const uint8_t *glyph = kUnknownGlyph;

        for (unsigned entry = 0; entry < sizeof(kGlyphs) / sizeof(kGlyphs[0]);
             ++entry)
        {
            if (kGlyphs[entry].character == character)
            {
                glyph = kGlyphs[entry].rows;
                break;
            }
        }

        for (unsigned row = 0; row < 7; ++row)
        {
            for (unsigned dy = 0; dy < scale; ++dy)
            {
                int target_y = y + (int)(row * scale + dy);

                if ((target_y < 0) || (target_y >= (int)height))
                {
                    continue;
                }

                for (unsigned column = 0; column < 5; ++column)
                {
                    if ((glyph[row] & (1U << (4 - column))) == 0)
                    {
                        continue;
                    }

                    for (unsigned dx = 0; dx < scale; ++dx)
                    {
                        int target_x =
                            x + (int)(index * 6 * scale + column * scale + dx);

                        if ((target_x < 0) || (target_x >= (int)width))
                        {
                            continue;
                        }

                        unsigned offset =
                            ((unsigned)target_y * width + (unsigned)target_x)
                            * 2;
                        pixels[offset] = color >> 8;
                        pixels[offset + 1] = (uint8_t)color;
                    }
                }
            }
        }
    }
}
