/**
 * @file test_lvgl_menu.c
 * @author Codex
 * @brief 使用真实LVGL及生产菜单检查刷新、焦点、缩略图和内存稳定性。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增无设备主机验收；法律信息：None。
 */
#include "ui/lvgl_menu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"

/* 整屏输出两侧的哨兵用于发现部分刷新越界；不打开任何设备节点。 */
static uint8_t s_guarded[MEDIA_LIST_BYTES + 16] = {0};
static uint32_t s_time = 0;

/**
 * @authors Codex
 * @brief 提供确定性的主机单调毫秒时钟。
 * @details 使用32位计数覆盖与固件相同的回绕语义。
 * 不调用系统sleep，菜单测试不受主机调度延迟影响。
 * 生产模块仅借用回调，不拥有或释放测试时钟。
 * @return 毫秒数；测试每次绘制前递增。
 */
static uint32_t Tick(void)
{
    return s_time;
}

/**
 * @authors Codex
 * @brief 检查完整屏幕保护区保持不变。
 * @details 前后各八字节覆盖flush区域坐标和stride计算错误。
 * 不以输出长度正确替代内存边界验证。
 * 每次绘制及不可交互消息后均重新检查。
 * 保护区不交给LVGL，只传入中间153600字节图像区。
 * @return None；保护区变化立即使测试失败。
 */
static void CheckGuards(void)
{
    for (unsigned index = 0; index < 8; ++index)
    {
        assert(0xa5 == s_guarded[index]);
        assert(0xa5 == s_guarded[MEDIA_LIST_BYTES + 8 + index]);
    }
}

/**
 * @authors Codex
 * @brief 找到根下原生lv_menu，确认当前页为真正lv_menu_page。
 * @details 根据对象class识别，不通过实现私有全局指针造假结果。
 * 同时检查菜单存在、当前页存在及正确页面类型。
 * 当前页由lv_menu管理，测试只能借用，禁止单独删除。
 * 新版菜单增加根标签时不影响查找顺序。
 * @return 当前菜单页面，借用LVGL对象。
 */
static lv_obj_t *CurrentPage(void)
{
    lv_obj_t *root = lv_display_get_screen_active(lv_display_get_default());

    for (unsigned index = 0; index < lv_obj_get_child_count(root); ++index)
    {
        lv_obj_t *object = lv_obj_get_child(root, (int32_t)index);

        if (lv_obj_check_type(object, &lv_menu_class))
        {
            lv_obj_t *page = lv_menu_get_cur_main_page(object);
            assert(NULL != page);
            assert(lv_obj_check_type(page, &lv_menu_page_class));
            return page;
        }
    }

    assert(false);
    return NULL;
}

/**
 * @authors Codex
 * @brief 用生产代码绘制并检查边界，保存可人工审查的RGB565图。
 * @details 固定容量正文可检查标签在320像素屏幕中的可读性。
 * 缓冲同样采用大端RGB565，FFmpeg按真实传输格式解码。
 * 无文件名时只渲染，不为每次压力循环创建截图。
 * 只有主机测试输出目录包含生成图片，不产生固件依赖。
 * 整幅图精确写入153600字节，短写和关闭失败均判测试失败。
 * 截图只验证软件输出，不能据此声称LCD物理观感验收通过。
 * @param ui 非空测试状态。
 * @param media 可为NULL的只读媒体模型。
 * @param name 可为NULL的输出文件名，只创建outputs中的主机文件。
 * @return None；失败即断言，不修改TF卡。
 */
static void Render(CameraUi *ui, const MediaListView *media, const char *name)
{
    s_time += 33;
    assert(LvglMenuRender(
        ui, media,
        "FS: exFAT\nTOTAL: 30400 MiB\nUSED: 400 MiB\nFREE: 30000 MiB", "",
        s_guarded + 8));
    CheckGuards();

    if (NULL != name)
    {
        FILE *file = fopen(name, "wb");
        assert(NULL != file);
        assert(MEDIA_LIST_BYTES
               == fwrite(s_guarded + 8, 1, MEDIA_LIST_BYTES, file));
        assert(0 == fclose(file));
    }
}

/**
 * @authors Codex
 * @brief 验证图像源转换和大端输出，每个缩略图像素精确对比。
 * @details 三个预览分别用不同梯度，可发现字节交换和重复图像缓存。
 * 使用LVGL计算坐标可同时发现菜单布局把缩略图裁剪的问题。
 * 每行每个字节比较，不仅核对少数角点或图像CRC。
 * 选中状态从真实对象读取，与纯状态机值交叉验证。
 * 最后一行使用损坏预览，检验图像失败后的可浏览占位分支。
 * 固定四项使末行也触及内容区底部，覆盖隐藏header的高度影响。
 * @param media 非空四行媒体模型。
 * @return None；使用LVGL真实对象坐标，不假定手写绘制布局。
 */
static void CheckThumbnails(const MediaListView *media)
{
    lv_obj_t *page = CurrentPage();
    assert(MEDIA_BROWSER_ROWS == lv_obj_get_child_count(page));

    for (unsigned row = 0; row < 3; ++row)
    {
        lv_obj_t *item = lv_obj_get_child(page, (int32_t)row);
        lv_obj_t *image = lv_obj_get_child(item, 0);
        assert(lv_obj_check_type(image, &lv_image_class));
        lv_area_t area = {0};
        lv_obj_get_coords(image, &area);
        assert((area.x1 >= 0) && (area.x2 < MEDIA_LIST_WIDTH));
        assert((area.y1 >= 28) && (area.y2 < 204));

        for (unsigned y = 0; y < MEDIA_THUMB_HEIGHT; ++y)
        {
            const uint8_t *actual =
                s_guarded + 8
                + (((area.y1 + y) * MEDIA_LIST_WIDTH + area.x1) * 2);
            assert(
                0
                == memcmp(actual,
                          media->thumbnails[row] + (y * MEDIA_THUMB_WIDTH * 2),
                          MEDIA_THUMB_WIDTH * 2));
        }
    }

    assert(lv_obj_has_state(lv_obj_get_child(page, 2), LV_STATE_FOCUSED));
    /* 第四行损坏预览必须有占位标签，仍有文件名而不是消失。 */
    lv_obj_t *last = lv_obj_get_child(page, 3);
    assert(lv_obj_check_type(lv_obj_get_child(last, 0), &lv_label_class));
}

/**
 * @authors Codex
 * @brief 验证所有页面、双确认高亮和重复生命周期，输出主机截图。
 * @details 先验证初始化及非法参数，防止测试只覆盖成功路径。
 * 双确认操作调用生产CameraUiHandle，确认页显示与授权共用状态。
 * CANCEL必须在两页都聚焦，确认默认取消不能产生擦除动作。
 * 缩略图更新复用同一地址，刻意测试图像缓存失效机制。
 * 预热后监控LVGL内建池，排除首次创建缓存影响稳定性比较。
 * 每周期切换五页，200周期覆盖1000次实际软件渲染。
 * 内存测试同时检查池结构，避免仅空闲字节相等掩盖堆损坏。
 * 删除及重新初始化验证清空对象引用，不依赖进程退出自动回收。
 * 结束才释放LVGL全局状态，模块Deinit不擅自删除其他显示。
 * @return EXIT_SUCCESS 全部通过；断言失败或超时表示测试失败。
 * @note 菜单代码无磁盘接口，确认测试只能产生动作，不能擦真实卡。
 */
int main(void)
{
    memset(s_guarded, 0xa5, sizeof(s_guarded));
    /* 空时钟不得创建显示；重复Init必须拒绝以免覆盖所有者。
     * 非菜单页面和空状态不得访问不存在的页面对象。
     * 本组检查发生在截图之前，防止后续成功路径掩盖状态污染。 */
    assert(!LvglMenuInit(NULL));
    assert(LvglMenuInit(Tick));
    assert(!LvglMenuInit(Tick));
    CameraUi ui = {0};
    CameraUiInit(&ui, false);
    assert(!LvglMenuRender(&ui, NULL, NULL, NULL, s_guarded + 8));
    assert(!LvglMenuRender(NULL, NULL, NULL, NULL, s_guarded + 8));

    /* 固定四行，前三行使用不同颜色梯度覆盖RGB565主机字节序转换。 */
    MediaListView media = {0};
    uint8_t thumbnails[3][MEDIA_THUMB_BYTES] = {{0}};
    const char *names[4] = {"IMG00001.BMP", "IMG00002.BMP", "VID00001.AVI",
                            "BROKEN.AVI"};
    media.page.count = 4;
    media.page.selected_row = 2;
    media.page.selected_index = 6;
    media.page.file_count = 13;

    for (unsigned row = 0; row < 4; ++row)
    {
        snprintf(media.page.names[row], MEDIA_BROWSER_NAME_BYTES, "%s",
                 names[row]);

        if (row < 3)
        {
            media.thumbnails[row] = thumbnails[row];
            media.thumbnail_valid[row] = true;

            for (unsigned pixel = 0;
                 pixel < (MEDIA_THUMB_WIDTH * MEDIA_THUMB_HEIGHT); ++pixel)
            {
                uint16_t color = (uint16_t)(pixel * 31 + row * 1000);
                thumbnails[row][pixel * 2] = color >> 8;
                thumbnails[row][pixel * 2 + 1] = (uint8_t)color;
            }
        }
    }

    /* 第一页选择TF项，真实焦点必须对应第二个表项。
     * 随后逐页导出，让标题、字体、警告和按键提示可供人工检查。
     * 所有截图使用同一软件显示，验证页面切换覆盖原图。 */
    ui.screen = UI_MENU;
    ui.menu_selected = 1;
    Render(&ui, NULL, "functions.rgb565");
    assert(
        lv_obj_has_state(lv_obj_get_child(CurrentPage(), 1), LV_STATE_FOCUSED));
    ui.screen = UI_TF_MENU;
    Render(&ui, NULL, "tf-card.rgb565");
    ui.screen = UI_CARD_STATUS;
    Render(&ui, NULL, "card-usage.rgb565");
    ui.screen = UI_FORMAT_PROMPT;
    ui.format_return = UI_TF_MENU;
    ui.format_yes = false;
    Render(&ui, NULL, "confirm-first.rgb565");
    /* 第一行是警告标签，CANCEL/YES分别为1/2，默认CANCEL高亮。 */
    assert(
        lv_obj_has_state(lv_obj_get_child(CurrentPage(), 1), LV_STATE_FOCUSED));
    /* 通过纯状态机第一次YES只进入第二页，不调用存储或格式化。
     * 第二页默认取消再次按确认必须返回TF页。
     * 这使显示层与授权层的两个CANCEL状态能够交叉核对。 */
    assert(UI_ACTION_NONE == CameraUiHandle(&ui, KEY_DOWN));
    assert(UI_ACTION_NONE == CameraUiHandle(&ui, KEY_CONFIRM));
    assert((UI_FORMAT_CONFIRM == ui.screen) && !ui.format_yes);
    Render(&ui, NULL, "confirm-second.rgb565");
    assert(
        lv_obj_has_state(lv_obj_get_child(CurrentPage(), 1), LV_STATE_FOCUSED));
    assert(!lv_obj_has_state(lv_obj_get_child(CurrentPage(), 2),
                             LV_STATE_FOCUSED));
    assert(UI_ACTION_NONE == CameraUiHandle(&ui, KEY_CONFIRM));
    assert(UI_TF_MENU == ui.screen);
    ui.screen = UI_BROWSER;
    Render(&ui, &media, "media.rgb565");
    CheckThumbnails(&media);

    /* 静态描述符地址不变、内容改变也必须刷新，不能命中旧图像缓存。 */
    memset(thumbnails[0], 0xf8, MEDIA_THUMB_BYTES);
    Render(&ui, &media, NULL);
    CheckThumbnails(&media);
    /* 缓存分配失败与目录为空分别呈现不同标签。
     * 两条路径都不得复用旧缩略图或访问空模型。
     * 不可交互消息同样检查输出保护区。 */
    Render(&ui, NULL, "media-memory-error.rgb565");
    MediaListView empty = {0};
    Render(&ui, &empty, "media-empty.rgb565");
    assert(LvglMenuMessage(
        "FORMATTING EXFAT...\nDO NOT REMOVE CARD\nDO NOT POWER OFF",
        s_guarded + 8));
    CheckGuards();

    /* 多次跨页切换应保持同样空闲内存，不能积累lv_menu历史或旧媒体行。 */
    for (unsigned cycle = 0; cycle < 10; ++cycle)
    {
        Render(&ui, &media, NULL);
    }

    lv_mem_monitor_t before = {0};
    lv_mem_monitor(&before);

    for (unsigned cycle = 0; cycle < 200; ++cycle)
    {
        ui.screen = UI_MENU;
        Render(&ui, NULL, NULL);
        ui.screen = UI_TF_MENU;
        Render(&ui, NULL, NULL);
        ui.screen = UI_CARD_STATUS;
        Render(&ui, NULL, NULL);
        ui.screen = UI_FORMAT_CONFIRM;
        Render(&ui, NULL, NULL);
        ui.screen = UI_BROWSER;
        Render(&ui, &media, NULL);
        CheckThumbnails(&media);
    }

    /* 空闲字节数比较选择同样的媒体页和同样的对象数量。
     * 每轮内部经历菜单历史清理与媒体行删除/重建。
     * 既检查净内存占用，也检查allocator自身的完整性。 */
    lv_mem_monitor_t after = {0};
    lv_mem_monitor(&after);
    assert(before.free_size == after.free_size);
    assert(LV_RESULT_OK == lv_mem_test());
    printf("PASS: 1000 menu renders; stable LVGL free bytes=%zu; pool=%zu\n",
           after.free_size, after.total_size);
    /* 让毫秒时钟跨过UINT32_MAX，页面刷新仍必须正确完成。 */
    s_time = UINT32_MAX - 10;
    Render(&ui, &media, NULL);
    CheckThumbnails(&media);
    LvglMenuDeinit();
    assert(!LvglMenuRender(&ui, &media, NULL, NULL, s_guarded + 8));
    LvglMenuDeinit();
    /* 删除显示后能重新创建同一模块，避免残留已删除对象指针。 */
    assert(LvglMenuInit(Tick));
    ui.screen = UI_MENU;
    Render(&ui, NULL, NULL);
    LvglMenuDeinit();
    /* 模块删除仅撤销本显示，全局LVGL回收留给应用所有者。
     * 测试确认重建成功后才结束全局生命周期。
     * 再次CheckGuards之前没有释放输出，避免检测使用悬空缓冲。 */
    CheckGuards();
    lv_deinit();
    puts("PASS: actual lv_menu pages, focus, thumbnails, guards, double "
         "confirmation and teardown");
    return EXIT_SUCCESS;
}
