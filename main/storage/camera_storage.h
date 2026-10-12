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

/**
 * @authors Codex
 * @brief 初始化 BOOT 输入并尝试挂载 TF；失败保持预览可用。
 * @return None；错误记录在串口，不自动格式化卡。
 */
void StorageInit(void);
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
#endif
