/**
 * @file camera_ui.c
 * @author Codex
 * @brief K2确认取消、K1上、K0下的模式与格式化确认逻辑。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：危险操作默认取消；法律信息：None。
 */
#include "camera_ui.h"

/**
 * @authors Codex
 * @brief 无需动态内存，创建默认拍照状态或格式提示。
 * @param ui 非空输出，初始化后归调用者持有。
 * @param unsupported 是否明确发现不支持文件系统。
 * @return None。
 */
void CameraUiInit(CameraUi *ui, bool unsupported)
{
    *ui = (CameraUi){.screen = unsupported ? UI_FORMAT_PROMPT : UI_PREVIEW,
                     .video_mode = false,
                     .format_yes = false,
                     .dirty = true};
}

/**
 * @authors Codex
 * @brief 两层确认页统一处理，进入下一层时再次默认取消。
 * @details
 * 第一层提示和第二层警告均以取消为默认选择。
 * 下键只选择YES，不直接执行任何磁盘动作。
 * 上键明确选择取消，不能切换到不可预测的选项。
 * 返回直接退出确认流程并清空YES状态。
 * 第一层YES加确认只进入第二层，不产生格式化动作。
 * 第二层再选YES加确认才返回UI_ACTION_FORMAT。
 * 状态机本身无磁盘接口，不能在测试期间意外擦真实卡。
 * 上层还要限制事件来源为物理键，拒绝串口模拟确认。
 * @param ui 非空状态，只能处于格式提示或确认页。
 * @param key 本次事件。
 * @return UI_ACTION_FORMAT仅在第二层YES确认时产生。
 */
static CameraUiAction FormatKey(CameraUi *ui, CameraKey key)
{
    if ((KEY_UP == key) || (KEY_DOWN == key))
    {
        /* K1向上选取消，K0向下选YES；不因长按而重复翻转选项。 */
        ui->format_yes = (KEY_DOWN == key);
        ui->dirty = true;
    }
    else if (KEY_CANCEL == key)
    {
        ui->screen = UI_PREVIEW;
        ui->format_yes = false;
        ui->dirty = true;
    }
    else if (KEY_CONFIRM == key)
    {
        if (!ui->format_yes)
        {
            ui->screen = UI_PREVIEW;
        }
        else if (UI_FORMAT_PROMPT == ui->screen)
        {
            /* 第一次YES不擦卡，第二层确认重新选取消。
             * 不允许继承上一页YES选择而一次连续确认完成擦除。 */
            ui->screen = UI_FORMAT_CONFIRM;
        }
        else
        {
            ui->screen = UI_PREVIEW;
            ui->format_yes = false;
            ui->dirty = true;
            return UI_ACTION_FORMAT;
        }

        ui->format_yes = false;
        ui->dirty = true;
    }

    return UI_ACTION_NONE;
}

/**
 * @authors Codex
 * @brief 按所在页面解释三键，存储模块负责实际副作用。
 * @details
 * 相同按键的含义由页面确定，不能在菜单中直接拍照。
 * 取景页K2拍照/录像确认，K1模式切换、K0进入浏览。
 * 菜单页K1向上/K0向下对应前后文件，K2打开媒体。
 * 打开媒体期间确认和返回都关闭并回到列表。
 * 从视频切换文件必须让上层先关闭音轨和释放缓冲。
 * 模式切换返回STOP动作，先封闭现有录像再更新显示。
 * 未知状态恢复预览，绝不将错误状态解释为格式化确认。
 * 所有状态数据归主任务，音频/输入任务不能并发修改。
 * @param ui 非空主任务私有状态。
 * @param key 本次短按、上下或即时长按事件。
 * @return 动作，未知状态返回关闭并恢复预览。
 */
CameraUiAction CameraUiHandle(CameraUi *ui, CameraKey key)
{
    if (KEY_NONE == key)
    {
        return UI_ACTION_NONE;
    }

    if ((UI_FORMAT_PROMPT == ui->screen) || (UI_FORMAT_CONFIRM == ui->screen))
    {
        return FormatKey(ui, key);
    }

    switch (ui->screen)
    {
    case UI_PREVIEW:
        if (KEY_CONFIRM == key)
        {
            return ui->video_mode ? UI_ACTION_RECORD : UI_ACTION_PHOTO;
        }

        if (KEY_UP == key)
        {
            ui->video_mode = !ui->video_mode;
            ui->dirty = true;
            return UI_ACTION_STOP;
        }

        if (KEY_DOWN == key)
        {
            ui->screen = UI_BROWSER;
            ui->dirty = true;
            return UI_ACTION_LIST;
        }

        return UI_ACTION_NONE;

    case UI_BROWSER:
        ui->dirty = true;

        if (KEY_CANCEL == key)
        {
            ui->screen = UI_PREVIEW;
            return UI_ACTION_CLOSE;
        }

        if (KEY_UP == key)
        {
            return UI_ACTION_PREVIOUS;
        }

        if (KEY_DOWN == key)
        {
            return UI_ACTION_NEXT;
        }

        return (KEY_CONFIRM == key) ? UI_ACTION_OPEN : UI_ACTION_NONE;

    case UI_PHOTO:
    case UI_VIDEO:
        /* 浏览图像或播放视频期间，确认/返回均关闭文件回到列表。
         * 上下先关闭正在播放的音轨，再选择相邻文件。
         * 格式化动作不可能从浏览页面产生。 */
        ui->screen = UI_BROWSER;
        ui->dirty = true;

        if (KEY_UP == key)
        {
            return UI_ACTION_PREVIOUS;
        }

        if (KEY_DOWN == key)
        {
            return UI_ACTION_NEXT;
        }

        return UI_ACTION_CLOSE;

    default:
        /* 状态异常恢复预览，不猜测用户是否确认格式化。 */
        ui->screen = UI_PREVIEW;
        ui->format_yes = false;
        ui->dirty = true;
        return UI_ACTION_CLOSE;
    }
}
