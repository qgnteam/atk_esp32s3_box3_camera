/**
 * @file board.h
 * @author Codex
 * @brief BOX3 供电、AW9523B 与共享 I2C 接口。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：按芯片 ID 验证扩展器；法律信息：None。
 */
#ifndef __BOARD_H_
#define __BOARD_H_

#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* 显示/采集统一使用像素单位，RGB565 每像素占两个字节。 */
#define PREVIEW_WIDTH       (320)
#define PREVIEW_HEIGHT      (240)
#define PREVIEW_FRAME_BYTES (PREVIEW_WIDTH * PREVIEW_HEIGHT * 2)

/**
 * @authors Codex
 * @brief 创建共享 I2C，验证 AW9523B 并设置已知供电引脚。
 * @param bus 非空输出；句柄由板级模块持有，调用者不可删除。
 * @return ESP_OK 或首个硬件错误；仅启动任务调用。
 */
esp_err_t BoardInit(i2c_master_bus_handle_t *bus);

/**
 * @authors Codex
 * @brief 创建 LCD 与 TF 共用 SPI2，并将两路片选保持高电平。
 * @return ESP_OK 或错误；仅启动调用一次，先于卡和 LCD 初始化。
 */
esp_err_t BoardInitSpi(void);

/**
 * @authors Codex
 * @brief 按 50 ms 低电平及 50 ms 释放执行摄像头复位。
 * @return ESP_OK 或 I2C 错误；仅采集完全停止时可调用。
 */
esp_err_t BoardResetCamera(void);

/**
 * @authors Codex
 * @brief 设置低有效背光，保持其他扩展 IO 状态。
 * @param enabled true 打开，false 关闭。
 * @return ESP_OK 或 I2C 错误；调用者须串行执行。
 */
esp_err_t BoardSetBacklight(bool enabled);

#endif
