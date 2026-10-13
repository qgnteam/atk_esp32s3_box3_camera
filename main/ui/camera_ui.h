/**
 * @file camera_ui.h
 * @author Codex
 * @brief 与硬件无关的相机菜单状态机，格式化必须两次确认。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：增加表驱动菜单与TF使用状况页面；法律信息：None。
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
    UI_VIDEO,
    UI_MENU,
    UI_TF_MENU,
    UI_CARD_STATUS,
    UI_SCREEN_COUNT /* 页面数量，供LVGL页面表边界检查使用。 */
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
    UI_ACTION_FORMAT,
    UI_ACTION_MENU,
    UI_ACTION_STATUS,
    UI_ACTION_FORMAT_REQUEST
} CameraUiAction;

typedef struct
{
    CameraUiScreen screen;
    bool video_mode;
    bool format_yes; /* 每次进入确认页必须重置为false，防止误擦除。 */
    CameraUiScreen
        format_return;      /* 主动格式化取消返回TF菜单，启动提示返回取景。 */
    unsigned menu_selected; /* 当前功能页的选中项，下标从零开始。 */
    bool dirty;             /* 文字界面变化时重绘，避免每帧重复读卡和刷菜单。 */
} CameraUi;

/* 菜单声明只保存静态字符串和目标；新增普通功能时扩展表即可。 */
typedef struct
{
    const char *label;     /* 静态英文标签，不分配或转移内存。 */
    CameraUiScreen target; /* 确认后进入的页面。 */
    CameraUiAction action; /* 交由主任务执行的副作用。 */
} CameraMenuItem;

/**
 * @authors Codex
 * @brief 获取功能页静态菜单表，状态机与绘制共用同一份定义。
 * @param screen 功能菜单页面。
 * @param count 非空输出菜单项数，无对应菜单时为零。
 * @return 借用静态表；未知页面返回NULL。
 */
const CameraMenuItem *CameraUiMenuItems(CameraUiScreen screen, unsigned *count);

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
