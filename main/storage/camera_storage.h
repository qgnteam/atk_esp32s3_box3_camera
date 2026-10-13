/**
 * @file camera_storage.h
 * @author Codex
 * @brief TF 卡拍照和录像的单任务控制接口。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：新增存储控制；法律信息：None。
 */
#ifndef __CAMERA_STORAGE_H_
#define __CAMERA_STORAGE_H_
#include "camera/camera_capture.h"
#include "driver/i2c_master.h"

/**
 * @authors Codex
 * @brief 初始化三键、音频、TF格式检测和UI；失败保持预览可用。
 * @param bus 有效共享I2C，不转移所有权。
 * @return None；格式化必须设备实体键双确认。
 */
void StorageInit(i2c_master_bus_handle_t bus);
/**
 * @authors Codex
 * @brief 轮询按键并同步保存图片或一帧录像。
 * @param frame 非空有效借用帧，只在返回前读取，不持有、不归还。
 * @return None；必须与初始化、停止在同一应用任务调用。
 */
void StorageProcess(const CameraFrame *frame);
/**
 * @authors Codex
 * @brief 完成索引、回填视频头并关闭文件；无录像时无操作。
 * @return None；在相机恢复或应用退出之前调用。
 */
void StorageStop(void);
/**
 * @authors Codex
 * @brief 查询录像状态供 LCD 显示红色标记。
 * @return true 表示正在录制；仅由同一主任务调用。
 */
bool StorageIsRecording(void);
/**
 * @authors Codex
 * @brief 根据当前模式显示预览、文件浏览或格式化确认页。
 * @param frame 非空有效借用帧，不转移所有权。
 * @return ESP_OK或LCD错误，等待所有DMA完成。
 */
esp_err_t StorageDraw(const CameraFrame *frame);
/**
 * @authors Codex
 * @brief 预览帧全部归还后执行待处理拍照并恢复预览。
 * @param bus 有效共享总线，不转移所有权。
 * @return 预览恢复结果，失败须停止相机。
 */
esp_err_t StorageCapturePending(i2c_master_bus_handle_t bus);
#endif
