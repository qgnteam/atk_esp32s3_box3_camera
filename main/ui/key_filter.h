/**
 * @file key_filter.h
 * @author Codex
 * @brief 三键去抖及K0/K2按住2秒即时长按，独立于GPIO和任务调度。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：增加K0物理长按事件供菜单取消；法律信息：None。
 */
#ifndef __KEY_FILTER_H_
#define __KEY_FILTER_H_
#include <stdbool.h>
#include <stdint.h>

/* 时间单位为微秒；长按从首次稳定按下序列的起点计时。
 * 去抖40ms，不额外增加长按阈值，K0/K2达到2秒立即产生各自长按事件。
 * K1只在松开时产生上移；K0长按供菜单取消，没有重复功能。 */
#define KEY_DEBOUNCE_US   (40000)
#define KEY_LONG_PRESS_US (2000000)

typedef enum
{
    KEY_NONE,
    KEY_CONFIRM,
    KEY_UP,
    KEY_DOWN,
    KEY_CANCEL,
    KEY_K0_CANCEL /* K0长按原始事件，由主任务按当前页面解释。 */
} CameraKey;

/* 物理键顺序对应GPIO0、XIO0、XIO1，不能当成UI事件编号。 */
typedef enum
{
    BUTTON_K0,
    BUTTON_K1,
    BUTTON_K2
} CameraButton;

typedef struct
{
    CameraKey short_event; /* 松开时的短按动作，K2为KEY_CONFIRM。 */
    int idle_level;        /* 松开电平，K0固定1，K1/K2由启动校准。 */
    int candidate_level;   /* 本次候选输入电平。 */
    int stable_level;      /* 已满足去抖时间的电平。 */
    int64_t changed_us;    /* 候选电平首次变化时间，微秒。 */
    int64_t pressed_us;    /* 按下序列起点时间，微秒。 */
    bool pressed;          /* 确认按下后置true，不依赖时间戳是否为零。 */
    bool long_sent;        /* 本次长按已经发出，松开不能补发短按。 */
} CameraKeyFilter;

/**
 * @authors Codex
 * @brief 初始化单键过滤器，不持有其他按键或任务资源。
 * @param filter 非空调用者持有的状态，初始化后串行采样。
 * @param idle_level 松开电平，0或1。
 * @param button 物理键K0/K1/K2；未知键不会产生动作。
 * @return None，尚无按下或长按事件。
 */
void CameraKeyFilterInit(CameraKeyFilter *filter, int idle_level,
                         CameraButton button);
/**
 * @authors Codex
 * @brief 去抖并检查K0/K2长按期限，达到2秒时主动返回对应长按事件一次。
 * @param filter 非空已初始化状态，不允许并发采样。
 * @param level 实际输入电平，0或1，必须是本轮成功读取结果。
 * @param now_us 单调递增采样时间，单位微秒。
 * @return 本轮事件；长按发出后持续按住和松开均返回KEY_NONE。
 * @note K0/K2不足2秒松开仍产生短按；采样周期10ms，截止点误差为一轮。
 */
CameraKey CameraKeyFilterSample(CameraKeyFilter *filter, int level,
                                int64_t now_us);
#endif
