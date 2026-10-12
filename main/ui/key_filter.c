/**
 * @file key_filter.c
 * @author Codex
 * @brief K2长按在按住期间触发，去抖释放不再重复确认。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：2秒期限与长按消费状态；法律信息：None。
 */
#include "key_filter.h"

/**
 * @authors Codex
 * @brief 将单键状态设为松开，时间戳为零也可正常开始计时。
 * @param filter 非空输出，由调用者持有。
 * @param idle_level 松开电平，0或1。
 * @param button 物理键编号，映射为下移/上移/确认。
 * @return None，不生成事件。
 */
void CameraKeyFilterInit(CameraKeyFilter *filter, int idle_level,
                         CameraButton button)
{
    CameraKey short_event = KEY_NONE;

    /* 映射属于输入层，不根据当前页面改写物理键，防止长按归属串键。 */
    switch (button)
    {
    case BUTTON_K0:
        short_event = KEY_DOWN;
        break;
    case BUTTON_K1:
        short_event = KEY_UP;
        break;
    case BUTTON_K2:
        short_event = KEY_CONFIRM;
        break;
    default:
        short_event = KEY_NONE;
        break;
    }

    *filter = (CameraKeyFilter){.short_event = short_event,
                                .idle_level = idle_level,
                                .candidate_level = idle_level,
                                .stable_level = idle_level};
}

/**
 * @authors Codex
 * @brief 处理一轮真实输入，长按判断独立于松开分支。
 * @details 按下满足40ms去抖后，以候选按下序列起点计时。
 * 候选已松开时不触发按住事件，避免1.99秒松开被误判成长按。
 * K2在候选与稳定电平均为按下时，到2秒立即发出一次取消。
 * long_sent在发送前置位，持续按住不重复发送，松开不补发确认。
 * 下一次确认按下才清空long_sent，支持连续多次短按/长按。
 * 释放去抖期间保留按下状态，使用首次释放时间判断真实时长。
 * 调度长时间中断而漏过期限时，释放也不能被降级为确认。
 * 状态机无GPIO、队列、打印或分配操作，主机执行与固件相同代码。
 * @param filter 非空且已初始化的串行状态。
 * @param level 本轮有效输入电平。
 * @param now_us 单调采样时间，单位微秒。
 * @return KEY_NONE、短按事件或一次KEY_CANCEL。
 */
CameraKey CameraKeyFilterSample(CameraKeyFilter *filter, int level,
                                int64_t now_us)
{
    if (level != filter->candidate_level)
    {
        filter->candidate_level = level;
        filter->changed_us = now_us;
    }

    if ((level != filter->stable_level)
        && ((now_us - filter->changed_us) >= KEY_DEBOUNCE_US))
    {
        filter->stable_level = level;

        if (level != filter->idle_level)
        {
            filter->pressed = true;
            filter->pressed_us = filter->changed_us;
            filter->long_sent = false;
        }
        else if (filter->pressed)
        {
            /* 长按消耗本次按下，抬起只清理状态，不再触发拍照或确认。
             * 时长取候选释放时间，释放去抖的40ms不能算进按住时长。 */
            bool sent = filter->long_sent;
            int64_t held_us = filter->changed_us - filter->pressed_us;
            filter->pressed = false;

            if (sent)
            {
                return KEY_NONE;
            }

            /* 极端采样中断后的释放保留长按语义，禁止误触发确认。
             * 正常10ms采样会在按住期间先发出KEY_CANCEL。 */
            if ((filter->short_event == KEY_CONFIRM)
                && (held_us >= KEY_LONG_PRESS_US))
            {
                filter->long_sent = true;
                return KEY_CANCEL;
            }

            return filter->short_event;
        }
    }

    /* 达到阈值立即触发，不等待稳定释放；同一次按住最多发出一次。
     * 若首次观察释放已过截止点，此轮也立即归为长按，不再等40ms。
     * 截止点前已观察到释放则等待短按去抖，不因时间继续增长误判。 */
    bool held_until_deadline =
        (filter->candidate_level != filter->idle_level)
        || ((filter->changed_us - filter->pressed_us) >= KEY_LONG_PRESS_US);
    if ((filter->short_event == KEY_CONFIRM) && filter->pressed
        && (filter->stable_level != filter->idle_level) && held_until_deadline
        && !filter->long_sent
        && ((now_us - filter->pressed_us) >= KEY_LONG_PRESS_US))
    {
        filter->long_sent = true;
        return KEY_CANCEL;
    }

    return KEY_NONE;
}
