/**
 * @file camera_capture.h
 * @author Codex
 * @brief GC0308 的有界等待、借用帧和归还接口。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：首版 DVP 采集；法律信息：None。
 */
#ifndef __CAMERA_CAPTURE_H_
#define __CAMERA_CAPTURE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* 借用帧只在 CameraReturn 前有效；禁止 free 或另一个任务持有。 */
typedef struct
{
    const uint8_t *data; /* 驱动拥有的 PSRAM 数据，RGB565 大端。 */
    size_t length;       /* 有效字节数，不能大于映射容量。 */
    size_t stride;       /* 每行字节跨度，不是像素数。 */
    unsigned width;      /* 当前帧宽度，像素。 */
    unsigned height;     /* 当前帧高度，像素。 */
    unsigned index;      /* 归还令牌，取到帧后不可修改。 */
    bool valid;          /* 完成标志与帧长度均通过检查。 */
} CameraFrame;

/**
 * @authors Codex
 * @brief 复用有效 I2C 总线初始化 GC0308 并开始 3 缓冲采集。
 * @param bus BoardInit 的有效句柄，摄像头不拥有该总线。
 * @return ESP_OK 或错误；失败由 CameraStop 回收已创建资源。
 */
esp_err_t CameraInit(i2c_master_bus_handle_t bus);
/**
 * @authors Codex
 * @brief 无借用帧且停止后初始化指定采集模式。
 * @param bus 有效共享总线，不转移所有权。
 * @param photo true 使用最大 VGA，false 使用 QVGA。
 * @return 初始化结果；失败须 CameraStop 回收。
 */
esp_err_t CameraInitMode(i2c_master_bus_handle_t bus, bool photo);

/**
 * @authors Codex
 * @brief 等待一帧，默认最长 1 秒；异常长度也返回可归还的无效帧。
 * @param frame 非空输出，成功后必须且只能 CameraReturn 一次。
 * @return ESP_OK、ESP_ERR_TIMEOUT 或驱动错误。
 */
esp_err_t CameraGet(CameraFrame *frame);

/**
 * @authors Codex
 * @brief 将成功取出的帧归还驱动，并清空借用指针。
 * @param frame 非空、尚未归还的帧；失败仍保持其借用状态。
 * @return ESP_OK 或错误；不允许并发或重复归还。
 */
esp_err_t CameraReturn(CameraFrame *frame);

/**
 * @authors Codex
 * @brief 设置 dequeue 等待时间以便非阻塞丢弃旧帧。
 * @param milliseconds 0 表示立即返回；正常采集设置为 1000。
 * @return ESP_OK 或驱动错误；只能由采集调用任务串行设置。
 */
esp_err_t CameraSetTimeout(unsigned milliseconds);

/**
 * @authors Codex
 * @brief 停流、关闭设备并释放驱动拥有的映射缓冲。
 * @return ESP_OK 或错误；只在无外部借用帧时调用。
 */
esp_err_t CameraStop(void);

/**
 * @authors Codex
 * @brief 无借用帧时停止采集DMA，保留设备、格式及映射缓冲。
 * @return ESP_OK已暂停，ESP_ERR_INVALID_STATE仍有借用帧，其他为驱动错误。
 * @note 主任务调用；失败不能假定DMA已停止，不释放在途资源。
 */
esp_err_t CameraPause(void);
/**
 * @authors Codex
 * @brief 将停流时清空的三个缓冲重新排队并恢复DMA采集。
 * @return ESP_OK采集中；失败由调用者停设备并执行有限恢复。
 * @note 不重置传感器或重新分配图像；仅主任务且无借用帧调用。
 */
esp_err_t CameraResume(void);
/**
 * @authors Codex
 * @brief 查询驱动采集状态，供主任务调度及只读诊断。
 * @return true表示STREAMON成功且尚未成功STREAMOFF。
 */
bool CameraIsStreaming(void);
/**
 * @authors Codex
 * @brief 查询累计成功取帧次数，暂停期间不得增加。
 * @return 累计次数，32位自然回绕；只在主任务读取。
 */
uint32_t CameraGetCount(void);
#endif
