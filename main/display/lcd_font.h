/**
 * @file lcd_font.h
 * @author Codex
 * @brief 无硬件依赖的5x7字体绘制，输出大端RGB565。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：从LCD条带字体提取供列表共用；法律信息：None。
 */
#ifndef __LCD_FONT_H_
#define __LCD_FONT_H_
#include <stdint.h>

/**
 * @authors Codex
 * @brief 将ASCII文字绘制到有界RGB565图像，自动裁剪边缘。
 * @param pixels 非空width*height*2字节目标，调用者持有。
 * @param width 图像宽度，像素，范围1至320。
 * @param height 图像高度，像素，范围1至240。
 * @param x 文字起点横坐标，像素，可以在图像边缘之外。
 * @param y 文字起点纵坐标，像素，可以为负数。
 * @param text 非空终止字符串，小写转换为大写。
 * @param scale 字形缩放倍率，范围1至2。
 * @param color 主机整数形式的RGB565前景色。
 * @return None，参数无效时不写入；仅调用者串行访问目标缓冲。
 */
void LcdFontDraw(uint8_t *pixels, unsigned width, unsigned height, int x, int y,
                 const char *text, unsigned scale, uint16_t color);
#endif
