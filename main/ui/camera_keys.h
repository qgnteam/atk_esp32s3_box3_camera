/**
 * @file camera_keys.h
 * @author Codex
 * @brief 三按键有界事件队列，避免写卡阻塞漏掉按键。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：K2达到2秒即产生长按，释放不补发确认；法律信息：None。
 */
#ifndef __CAMERA_KEYS_H_
#define __CAMERA_KEYS_H_
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "key_filter.h"

/* K2短按松开确认，按住2秒即时取消；抬起不再补发确认。 */
/**
 * @authors Codex
 * @brief 启动去抖任务，先记录松开电平，禁止上电按住触发。
 * @return ESP_OK或初始化错误；只调用一次。
 */
esp_err_t CameraKeysInit(void);
/**
 * @authors Codex
 * @brief 主任务零等待取一项事件。
 * @return KEY_NONE或按键事件；队列所有权保留在模块。
 */
CameraKey CameraKeysRead(void);
#endif
