/**
 * @file camera_ui.h
 * @author Codex
 * @brief 与硬件无关的相机菜单状态机，格式化必须两次确认。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增状态和动作；法律信息：None。
 */
#ifndef __CAMERA_UI_H_
#define __CAMERA_UI_H_
#include "camera_keys.h"

/* 状态机只产生动作，不访问磁盘，因此可在主机验证危险操作门槛。 */
typedef enum
{
    UI_PREVIEW,
    UI_FORMAT_PROMPT,
    UI_FORMAT_CONFIRM,
    UI_BROWSER,
    UI_PHOTO,
    UI_VIDEO
} CameraUiScreen;

typedef enum
{
    UI_ACTION_NONE,
    UI_ACTION_PHOTO,
    UI_ACTION_RECORD,
    UI_ACTION_STOP,
    UI_ACTION_LIST,
    UI_ACTION_PREVIOUS,
    UI_ACTION_NEXT,
    UI_ACTION_OPEN,
    UI_ACTION_CLOSE,
    UI_ACTION_FORMAT
} CameraUiAction;

typedef struct
{
    CameraUiScreen screen;
    bool video_mode;
    bool format_yes; /* 每次进入确认页必须重置为false，防止误擦除。 */
    bool dirty;      /* 文字界面变化时重绘，避免每帧重复读卡和刷菜单。 */
} CameraUi;

/**
 * @authors Codex
 * @brief 初始化拍照模式或不支持格式的提示页，默认取消。
 * @param ui 非空调用者拥有的状态。
 * @param unsupported true进入格式提示，false正常预览。
 * @return None。
 */
void CameraUiInit(CameraUi *ui, bool unsupported);
/**
 * @authors Codex
 * @brief 消费一次按键，返回由主任务执行的动作。
 * @param ui 非空状态，不可并发访问。
 * @param key 经过消抖的短按/上下或即时长按事件，KEY_NONE无操作。
 * @return 动作；只有两个页面分别选择YES并确认才产生FORMAT。
 */
CameraUiAction CameraUiHandle(CameraUi *ui, CameraKey key);
#endif
