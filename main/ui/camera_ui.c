/**
 * @file camera_ui.c
 * @author Codex
 * @brief 按页面映射三键的模式与格式化确认逻辑。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：按页面将菜单K0映射确认/取消、K1/K2映射下上；法律信息：None。
 */
#include "camera_ui.h"
#include <stddef.h>

/* 最新修改：一级菜单末项显式返回取景，复用关闭动作及资源回收。 */
static const CameraMenuItem kMainMenu[] = {
    {"MEDIA VIEW", UI_BROWSER, UI_ACTION_LIST},
    {"TF CARD", UI_TF_MENU, UI_ACTION_MENU},
    {"BACK TO PREVIEW", UI_PREVIEW, UI_ACTION_CLOSE}};

/* 格式化只请求进入确认流程，菜单表不能直接产生擦除动作。 */
static const CameraMenuItem kTfMenu[] = {
    {"CARD USAGE", UI_CARD_STATUS, UI_ACTION_STATUS},
    {"FORMAT TF CARD", UI_FORMAT_PROMPT, UI_ACTION_FORMAT_REQUEST}};

/**
 * @authors Codex
 * @brief 借用菜单表；未知页不提供可执行菜单项。
 * @param screen 页面编号。
 * @param count 非空数量输出。
 * @return 静态菜单表或NULL，不需释放。
 */
const CameraMenuItem *CameraUiMenuItems(CameraUiScreen screen, unsigned *count)
{
    *count = 0;

    if (UI_MENU == screen)
    {
        *count = sizeof(kMainMenu) / sizeof(kMainMenu[0]);
        return kMainMenu;
    }

    if (UI_TF_MENU == screen)
    {
        *count = sizeof(kTfMenu) / sizeof(kTfMenu[0]);
        return kTfMenu;
    }

    return NULL;
}

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
                     .format_return = UI_PREVIEW,
                     .menu_selected = 0,
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
        /* K2向上选取消，K1向下选YES；不因长按而重复翻转选项。 */
        ui->format_yes = (KEY_DOWN == key);
        ui->dirty = true;
    }
    else if (KEY_CANCEL == key)
    {
        ui->screen = ui->format_return;
        ui->format_yes = false;
        ui->dirty = true;
    }
    else if (KEY_CONFIRM == key)
    {
        if (!ui->format_yes)
        {
            ui->screen = ui->format_return;
        }
        else if (UI_FORMAT_PROMPT == ui->screen)
        {
            /* 第一次YES不擦卡，第二层确认重新选取消。
             * 不允许继承上一页YES选择而一次连续确认完成擦除。 */
            ui->screen = UI_FORMAT_CONFIRM;
        }
        else
        {
            ui->screen = ui->format_return;
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
 * @brief 在主任务消费处解释实体键，不让输入任务读取UI共享状态。
 * @details K0进入菜单的事件只在取景解释一次，不会再次确认首项。
 * 菜单中K2长按不取消；导航仅接受短按，防止持键跨页动作。
 * K0长按在菜单取消，在取景不触发进入菜单。
 * @param screen 当前页面，包括启动时的格式提示页。
 * @param key 原始物理事件，串口模拟事件不得传入。
 * @return 转换后的逻辑事件，未分配的长按返回KEY_NONE。
 */
CameraKey CameraUiPhysicalKey(CameraUiScreen screen, CameraKey key)
{
    if (UI_PREVIEW == screen)
    {
        return (KEY_K0_CANCEL == key) ? KEY_NONE : key;
    }

    /* 最新修改：菜单K1下移、K2上移，取景布局及格式化双确认门槛保持一致。 */
    switch (key)
    {
    case KEY_DOWN:
        return KEY_CONFIRM;
    case KEY_CONFIRM:
        return KEY_UP;
    case KEY_UP:
        return KEY_DOWN;
    case KEY_K0_CANCEL:
        return KEY_CANCEL;
    case KEY_CANCEL:
        return KEY_NONE;
    default:
        return key;
    }
}

/**
 * @authors Codex
 * @brief 按所在页面解释三键，存储模块负责实际副作用。
 * @details
 * 相同按键的含义由页面确定，不能在菜单中直接拍照。
 * 取景页K2拍照/录像确认，K1模式切换、K0进入功能选择。
 * 菜单页K1向下/K2向上对应前后文件，K0打开媒体。
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
            ui->screen = UI_MENU;
            ui->menu_selected = 0;
            ui->dirty = true;
            return UI_ACTION_MENU;
        }

        return UI_ACTION_NONE;

    case UI_MENU:
    case UI_TF_MENU:
    {
        /* 每页默认第一项；取消逐级返回，不直接执行任何存储动作。 */
        unsigned count = 0;
        const CameraMenuItem *items = CameraUiMenuItems(ui->screen, &count);
        ui->dirty = true;

        if (KEY_CANCEL == key)
        {
            ui->screen = (UI_TF_MENU == ui->screen) ? UI_MENU : UI_PREVIEW;
            ui->menu_selected = (UI_MENU == ui->screen) ? 1 : 0;
            return UI_ACTION_CLOSE;
        }

        if ((KEY_UP == key) || (KEY_DOWN == key))
        {
            ui->menu_selected = (KEY_DOWN == key)
                                    ? ((ui->menu_selected + 1) % count)
                                    : ((ui->menu_selected + count - 1) % count);
        }
        else if ((KEY_CONFIRM == key) && (ui->menu_selected < count))
        {
            const CameraMenuItem *item = &items[ui->menu_selected];
            ui->screen = item->target;
            ui->menu_selected = 0;
            ui->format_yes = false;
            ui->format_return = UI_TF_MENU;
            return item->action;
        }

        return UI_ACTION_NONE;
    }

    case UI_CARD_STATUS:
        if ((KEY_CONFIRM == key) || (KEY_CANCEL == key))
        {
            ui->screen = UI_TF_MENU;
            ui->menu_selected = 0;
            ui->dirty = true;
            return UI_ACTION_CLOSE;
        }

        return UI_ACTION_NONE;

    case UI_BROWSER:
        ui->dirty = true;

        if (KEY_CANCEL == key)
        {
            ui->screen = UI_MENU;
            ui->menu_selected = 0;
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
