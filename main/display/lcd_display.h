/**
 * @file lcd_display.h
 * @author Codex
 * @brief ST7789 同步帧提交接口，内部使用 DMA 条缓冲。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：首版实现；法律信息：None。
 */
#ifndef __LCD_DISPLAY_H_
#define __LCD_DISPLAY_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @authors Codex
 * @brief 在已创建的 SPI2 上初始化 ST7789 和内部 DMA 缓冲。
 * @return ESP_OK 或错误；只由预览任务初始化一次。
 */
esp_err_t LcdInit(void);

/**
 * @authors Codex
 * @brief 把 RGB565 大端帧分条拷贝至 LCD，等待最后一次 DMA 完成。
 * @param frame 非空且至少包含 240 行，所有权仍在调用者。
 * @param stride 每行字节数，至少为 640。
 * @param recording true 时在取景右上角绘制红色录像标记。
 * @return ESP_OK 或传输错误；超时不复用缓冲，须重启恢复。
 * @note 只能由同一个任务调用；返回后摄像头帧可归还。
 */
esp_err_t LcdDrawFrame(const uint8_t *frame, size_t stride, bool recording);

/**
 * @authors Codex
 * @brief 显示单色诊断图，验证全屏 DMA 完成。
 * @param rgb565 主机整数形式的 RGB565 颜色。
 * @return ESP_OK 或错误；仅同一预览任务调用。
 */
esp_err_t LcdFill(uint16_t rgb565);

#endif
