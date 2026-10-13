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

/**
 * @authors Codex
 * @brief 设置取景及回放上方状态文字，不修改原始文件像素。
 * @param caption 非空ASCII字符串，最多39字符，复制后不持有。
 * @return None，仅主任务调用。
 */
void LcdSetCaption(const char *caption);
/**
 * @authors Codex
 * @brief 显示最多8行文字界面，背景深蓝。
 * @param lines 非空以换行分隔的ASCII文本，每行最多26个可见字符。
 * @return ESP_OK或LCD错误，必须在LCD初始化后调用。
 */
esp_err_t LcdShowText(const char *lines);
/**
 * @authors Codex
 * @brief 显示已经校正方向的RGB565大端图片，不再水平翻转。
 * @param pixels 非空320x240x2字节，只借用至返回。
 * @return ESP_OK或LCD错误，等待全部DMA完成。
 */
esp_err_t LcdDrawImage(const uint8_t *pixels);
/**
 * @authors Codex
 * @brief 设置快门白色反馈，不改变相机帧。
 * @param enabled 是否启用白色混合，仅主任务调用。
 * @return None。
 */
void LcdSetFlash(bool enabled);
/**
 * @authors Codex
 * @brief 保持纯色诊断图，可反转坐标并核对每条发送缓冲。
 * @param rgb565 主机序RGB565颜色；不叠加标题或快门反馈。
 * @param reversed true旋转180度，false恢复正常面板方向。
 * @return ESP_OK表示缓冲一致且DMA完成；错误由调用者停止绘制。
 * @note 仅主任务在无录像/文件操作时调用，不读取LCD像素或检测面板缺陷。
 */
esp_err_t LcdDiagnosticFill(uint16_t rgb565, bool reversed);
#endif
