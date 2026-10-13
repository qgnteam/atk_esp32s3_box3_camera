/**
 * @file lvgl_menu.c
 * @author Codex
 * @brief 使用 LVGL lv_menu、标签和图像绘制三键相机菜单。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增单任务LVGL适配，缩略图转主机字节序；法律信息：LVGL按上游MIT许可证使用。
 */
#include "lvgl_menu.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"

/* 刷新只允许主任务同步执行，不能用另一个绘制线程访问借用输出。
 * 配置偏离时在构建期拒绝，避免固件出现难以重现的跨任务访问。 */
_Static_assert(LV_USE_OS == LV_OS_NONE, "BOX3 menus require single-task LVGL");
_Static_assert(LV_DRAW_SW_DRAW_UNIT_CNT == 1,
               "BOX3 menus require one draw unit");

/* 固定320x240横屏：标题28像素，内容176像素，底部36像素。
 * 媒体四行各44像素，48x36缩略图保留原始比例。
 * 12行局部绘制缓冲只供软件渲染，不交给SPI DMA。
 * 传输沿用LCD模块的内部DMA双条缓冲和完成信号。 */
#define MENU_HEADER_HEIGHT  (28)
#define MENU_CONTENT_HEIGHT (176)
#define MENU_ROW_HEIGHT     (44)
#define MENU_DRAW_ROWS      (12)
#define MENU_PAGE_COUNT     (UI_SCREEN_COUNT)

/* 所有LVGL对象由主任务独占，静态缓冲不跨任务读写。
 * 图像描述符和像素必须在对象引用期间保持存活。
 * 缩略图复制进本模块，媒体缓存释放不使LVGL持有悬空指针。 */
static lv_display_t *s_display = NULL;
static lv_obj_t *s_menu = NULL;
static lv_obj_t *s_header = NULL;
static lv_obj_t *s_footer = NULL;
static lv_obj_t *s_pages[MENU_PAGE_COUNT] = {NULL};
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_format_rows[2][2] = {{NULL}};
static lv_obj_t *s_message_page = NULL;
static lv_obj_t *s_message_label = NULL;
static uint8_t *s_output = NULL;
static _Alignas(4) uint8_t
    s_draw_buffer[MEDIA_LIST_WIDTH * MENU_DRAW_ROWS * 2] = {0};
static uint16_t s_thumbnail_pixels[MEDIA_BROWSER_ROWS]
                                  [MEDIA_THUMB_WIDTH * MEDIA_THUMB_HEIGHT] = {
                                      {0}};
static lv_image_dsc_t s_thumbnail_images[MEDIA_BROWSER_ROWS] = {0};

/**
 * @authors Codex
 * @brief 将局部主机字节序RGB565转为整屏大端输出，立即归还软件缓冲。
 * @details 显示配置固定RGB565，每像素两字节；禁止把RGB888传入本回调。
 * 对齐stride来自当前LVGL绘制缓冲，不以区域宽度推测。
 * memcpy读取16位颜色可处理非对齐输入，避免未对齐访问故障。
 * 区域坐标已由LVGL裁剪，输出容量由公开接口要求保证。
 * 调用者必须在整个刷新期间保持输出指针和缓冲存活。
 * 刷新前后不改变LCD片选，文件读写可由上层保持串行。
 * 输出为空的初始化刷新仍需归还缓冲，不能让软件渲染等待。
 * @param display 本模块显示，LVGL提供。
 * @param area 已裁剪到显示范围的刷新区域，单位像素。
 * @param pixels 已完成软件绘制的借用缓冲，按实际stride读取。
 * @return None；不提交SPI，不阻塞，不持有pixels。
 * @note LVGL flush_ready只表示复制完成，真实LCD由上层随后同步显示。
 */
static void FlushPixels(lv_display_t *display, const lv_area_t *area,
                        uint8_t *pixels)
{
    const uint32_t stride = lv_display_get_buf_active(display)->header.stride;

    if (NULL != s_output)
    {
        /* 区域宽度可能不是320，不能假设每行连续无对齐填充。
         * 输出只在Render期间借用，调用返回前所有刷新都已完成。 */
        for (int32_t y = area->y1; y <= area->y2; ++y)
        {
            const uint8_t *source = pixels + ((y - area->y1) * stride);
            uint8_t *destination =
                s_output + ((y * MEDIA_LIST_WIDTH + area->x1) * 2);

            for (int32_t x = area->x1; x <= area->x2; ++x)
            {
                uint16_t color = 0;
                memcpy(&color, source + ((x - area->x1) * 2), sizeof(color));
                destination[(x - area->x1) * 2] = color >> 8;
                destination[((x - area->x1) * 2) + 1] = (uint8_t)color;
            }
        }
    }

    lv_display_flush_ready(display);
}

/**
 * @authors Codex
 * @brief 设置无阴影深色容器，避免默认主题动画及额外绘制层。
 * @details 不透明背景确保上一相机画面不会透入菜单。
 * 删除主题style也会删除布局属性，调用者随后明确设置flex。
 * 文本样式向子标签继承，避免每个标签重复分配同类style。
 * 滚动条不显示，但菜单仍可立即滚动到超出可见区的选中项。
 * @param object 非空新对象。
 * @return None；删除默认style后显式指定必要的布局和颜色。
 */
static void StyleContainer(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
}

/**
 * @authors Codex
 * @brief 创建拷贝字符串的标签，可设置固定宽度避免长名称溢出。
 * @details 使用复制接口，使栈上正文在渲染返回后可以失效。
 * DOT模式限制长文件名，禁止滚动文字引入后台动画。
 * 多行正文使用显式换行，状态信息不依赖自动文字轮播。
 * 父对象销毁会释放标签，不向调用者转移单独释放责任。
 * @param parent 非空父对象，拥有返回对象。
 * @param text 非空文本，复制后不持有。
 * @param width 标签宽度，像素。
 * @return 新标签指针；分配失败返回NULL。
 */
static lv_obj_t *CreateLabel(lv_obj_t *parent, const char *text, int32_t width)
{
    lv_obj_t *label = lv_label_create(parent);

    if (NULL != label)
    {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, text);
    }

    return label;
}

/**
 * @authors Codex
 * @brief 创建LVGL菜单页面，标题由独立固定页眉显示。
 * @details 无标题页面避免lv_menu内部header占用额外内容高度。
 * lv_menu自动把未显示页面移入隐藏存储容器，不能手动free。
 * 页宽使用显示像素数，页高由当前主内容区的flex分配。
 * 页面编号沿用状态机，数组仅接受本模块明确创建的页面。
 * @param screen 有效页面枚举；对象在display删除时自动释放。
 * @return 新页面，失败为NULL。
 */
static lv_obj_t *CreatePage(CameraUiScreen screen)
{
    lv_obj_t *page = lv_menu_page_create(s_menu, NULL);

    if (NULL != page)
    {
        StyleContainer(page);
        lv_obj_set_width(page, MEDIA_LIST_WIDTH);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(page, 0, 0);
        s_pages[screen] = page;
    }

    return page;
}

/**
 * @authors Codex
 * @brief 创建可高亮的原生菜单行，通过状态机同步焦点。
 * @details 44像素行高允许四项同时显示，不为所有目录文件创建对象。
 * 聚焦样式表示状态机已选中，GUI本身不接收第二份输入队列。
 * 两像素上下padding保证36像素缩略图在选中边框内完整显示。
 * 使用flex水平排列图像和文本，可继续添加状态图标。
 * 行本身不滚动，超长功能列表由父菜单页负责滚动。
 * 未设置style transition，选中变化一次同步绘制即完成。
 * @param page 非空菜单页。
 * @return 菜单行，分配失败为NULL。
 * @note 本对象不连接擦除回调，存储动作只能经CameraUiHandle授权。
 */
static lv_obj_t *CreateRow(lv_obj_t *page)
{
    lv_obj_t *row = lv_menu_cont_create(page);

    if (NULL != row)
    {
        StyleContainer(row);
        lv_obj_set_size(row, MEDIA_LIST_WIDTH, MENU_ROW_HEIGHT);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_style_pad_top(row, 2, 0);
        lv_obj_set_style_pad_bottom(row, 2, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x174e70),
                                  LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(row, lv_color_hex(0x26d7ef),
                                      LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(row, 2, LV_STATE_FOCUSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    }

    return row;
}

/**
 * @authors Codex
 * @brief 按菜单表创建功能行，数量及顺序与状态机自动一致。
 * @details 表内字符串在固件生命周期内有效，标签仍复制其内容。
 * 初始化时创建所有普通功能行，之后移动选择仅修改对象状态。
 * 失败返回后由初始化统一删除整个display，避免逐行漏释放。
 * 当前菜单表只定义功能，存储层负责打开媒体及查询卡容量。
 * @param screen 功能菜单或TF菜单。
 * @return true全部创建，false分配失败。
 */
static bool CreateFunctionPage(CameraUiScreen screen)
{
    unsigned count = 0;
    const CameraMenuItem *items = CameraUiMenuItems(screen, &count);
    lv_obj_t *page = CreatePage(screen);

    if (NULL == page)
    {
        return false;
    }

    /* 添加功能只需扩展表和动作处理，无需另写LVGL行布局。
     * 不通过lv_menu自动点击执行动作，防止绕过物理双确认。 */
    for (unsigned index = 0; index < count; ++index)
    {
        lv_obj_t *row = CreateRow(page);

        if ((NULL == row)
            || (NULL == CreateLabel(row, items[index].label, 296)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @authors Codex
 * @brief 创建两页相互独立的确认选择，YES不继承上页状态。
 * @details 第一次正文说明目标文件系统，第二次正文说明全数据丢失。
 * 提示页与警告页分别拥有标签和选择对象，不能共享YES高亮状态。
 * 64像素警告区加两个44像素选择仍在176像素内容区内。
 * 黄色正文与青色焦点区分警告信息和当前选择。
 * 选择对象没有磁盘或按键回调，无法直接调用格式化原语。
 * @param screen 格式提示或最终警告页面。
 * @param stage 从零开始的确认层，0或1。
 * @return true完成，false分配失败。
 */
static bool CreateFormatPage(CameraUiScreen screen, unsigned stage)
{
    lv_obj_t *page = CreatePage(screen);

    if (NULL == page)
    {
        return false;
    }

    const char *warning =
        (0 == stage)
            ? "Confirmation 1 of 2\nFormat TF card as exFAT?"
            : "Confirmation 2 of 2\nERASE ALL DATA - files will be lost";
    lv_obj_t *label = CreateLabel(page, warning, 312);

    if (NULL == label)
    {
        return false;
    }

    lv_obj_set_height(label, 64);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffce70), 0);

    /* 两个页面均用独立CANCEL/YES行；高亮来自format_yes。
     * LVGL只显示选择，不可自己格式化或把按住事件解释为确认。 */
    for (unsigned choice = 0; choice < 2; ++choice)
    {
        lv_obj_t *row = CreateRow(page);
        s_format_rows[stage][choice] = row;

        if ((NULL == row)
            || (NULL
                == CreateLabel(
                    row, (0 == choice) ? "CANCEL" : "YES - FORMAT EXFAT", 296)))
        {
            return false;
        }
    }

    return true;
}

/**
 * @authors Codex
 * @brief 根据当前状态更新列表高亮，不引入焦点动画。
 * @details 先移除每行焦点，避免旧页与新页同时呈现两个选中项。
 * 越界选中值只清空高亮，不读取越界对象，也不改变菜单授权。
 * 只操作功能行/媒体行；确认页还包含警告标签，独立处理。
 * 页面切换后会再次按实际布局滚动，保证隐藏页也正确定位。
 * @param page 非空页，其直接子对象都是功能行。
 * @param selected 选中行下标，越界时不高亮。
 * @return None；滚动立即完成，首次显示可以看见选中项。
 */
static void SelectRow(lv_obj_t *page, unsigned selected)
{
    const unsigned count = lv_obj_get_child_count(page);

    for (unsigned row = 0; row < count; ++row)
    {
        lv_obj_t *item = lv_obj_get_child(page, (int32_t)row);
        lv_obj_remove_state(item, LV_STATE_FOCUSED);

        if (row == selected)
        {
            lv_obj_add_state(item, LV_STATE_FOCUSED);
            lv_obj_scroll_to_view(item, LV_ANIM_OFF);
        }
    }
}

/**
 * @authors Codex
 * @brief 创建一次性显示及固定菜单对象，不启动独立刷新线程。
 * @details tick由上层提供，主机测试可复用真实代码和确定时钟。
 * 固件采用LV_OS_NONE和一个软件绘制单元，所有GUI调用归主任务。
 * LVGL内建64KiB池限制对象内存，不因TF中文件数量扩大。
 * draw buffer明确四字节对齐，满足RGB565软件绘制要求。
 * 只创建菜单显示，不注册第二个LCD驱动或重复配置共享SPI。
 * 后续取景直接写LCD，返回菜单时全屏失效以刷新旧相机画面。
 * @param tick 非空单调毫秒时钟。
 * @return true成功，false失败；失败资源由Deinit撤销。
 * @note 禁用主题，采用项目自己的深色样式；上游许可证保持原样。
 */
bool LvglMenuInit(uint32_t (*tick)(void))
{
    if ((NULL == tick) || (NULL != s_display))
    {
        return false;
    }

    lv_init();
    lv_tick_set_cb(tick);
    s_display = lv_display_create(MEDIA_LIST_WIDTH, MEDIA_LIST_HEIGHT);

    if (NULL == s_display)
    {
        return false;
    }

    /* 原生RGB565仅在flush转一次大端，摄像头和文件缓冲不受影响。
     * 显式使用单软件绘制缓冲，主任务同步调用lv_refr_now完成绘制。 */
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL,
                           sizeof(s_draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_display, FlushPixels);
    lv_display_set_theme(s_display, NULL);
    lv_obj_t *root = lv_display_get_screen_active(s_display);
    StyleContainer(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_header = CreateLabel(root, "FUNCTIONS", 312);
    s_footer = CreateLabel(root, "", 312);
    s_menu = lv_menu_create(root);

    if ((NULL == s_header) || (NULL == s_footer) || (NULL == s_menu))
    {
        LvglMenuDeinit();
        return false;
    }

    lv_obj_set_pos(s_header, 4, 6);
    lv_obj_set_pos(s_footer, 4, MENU_HEADER_HEIGHT + MENU_CONTENT_HEIGHT + 2);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_12, 0);
    lv_obj_set_height(s_footer, 34);
    StyleContainer(s_menu);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_ROW);
    lv_obj_set_pos(s_menu, 0, MENU_HEADER_HEIGHT);
    lv_obj_set_size(s_menu, MEDIA_LIST_WIDTH, MENU_CONTENT_HEIGHT);
    lv_menu_set_mode_root_back_button(s_menu,
                                      LV_MENU_ROOT_BACK_BUTTON_DISABLED);

    /* 功能菜单和TF子菜单为原生lv_menu_page，状态机决定当前页面。
     * 清空库历史防止外部状态导航与lv_menu内部返回栈分歧。
     * 保留可扩展的表定义，而不是将功能标签散落在渲染代码中。 */
    bool success = CreateFunctionPage(UI_MENU) && CreateFunctionPage(UI_TF_MENU)
                   && (NULL != CreatePage(UI_BROWSER))
                   && (NULL != CreatePage(UI_CARD_STATUS))
                   && CreateFormatPage(UI_FORMAT_PROMPT, 0)
                   && CreateFormatPage(UI_FORMAT_CONFIRM, 1);

    if (success)
    {
        s_detail = CreateLabel(s_pages[UI_CARD_STATUS], "", 308);
        s_message_page = lv_menu_page_create(s_menu, NULL);
        success = (NULL != s_detail) && (NULL != s_message_page);
    }

    if (success)
    {
        StyleContainer(s_message_page);
        s_message_label = CreateLabel(s_message_page, "", 308);
        success = NULL != s_message_label;
    }

    if (!success)
    {
        LvglMenuDeinit();
    }

    return success;
}

/**
 * @authors Codex
 * @brief 构造媒体页，使用LVGL图像组件显示本页缓存缩略图。
 * @details 最多复制四个预览，转为主机RGB565后使用静态图像描述符。
 * 文件名按31字符限制复制，避免目录变更或模型损坏导致越界读。
 * 空页保留通知页脚，内存不足与无文件使用不同正文。
 * 本函数不重新读取TF，页内移动仍使用存储层的缩略图缓存。
 * 图像描述符地址复用，更新前清空图像缓存以消除旧数据命中。
 * 重新创建媒体行时先删除旧对象，内存不随选择次数累积。
 * @param media 借用本页模型，可为NULL表示缓存分配失败。
 * @return true完成，false分配失败。
 * @note 所有文本和缩略图在本函数复制，不保留模型指针。
 */
static bool UpdateMedia(const MediaListView *media)
{
    lv_obj_t *page = s_pages[UI_BROWSER];
    /* 先删除图像组件，随后才更新它们引用的静态描述符。
     * 页面最多四行，内存与卡中文件数无关，不缓存整张目录。 */
    lv_obj_clean(page);
    lv_image_cache_drop(NULL);

    if ((NULL == media) || (0 == media->page.count))
    {
        return NULL
               != CreateLabel(page,
                              (NULL == media) ? "PREVIEW MEMORY ERROR"
                                              : "NO MEDIA FILES",
                              308);
    }

    for (unsigned row = 0;
         (row < media->page.count) && (row < MEDIA_BROWSER_ROWS); ++row)
    {
        lv_obj_t *item = CreateRow(page);

        if (NULL == item)
        {
            return false;
        }

        /* 缩略图是大端RGB565，LVGL图像源要求主机字节序。
         * 转换限定48x36像素，不触碰原照片、录像或音轨文件。 */
        if (media->thumbnail_valid[row] && (NULL != media->thumbnails[row]))
        {
            for (unsigned pixel = 0;
                 pixel < (MEDIA_THUMB_WIDTH * MEDIA_THUMB_HEIGHT); ++pixel)
            {
                const uint8_t *source = media->thumbnails[row] + (pixel * 2);
                s_thumbnail_pixels[row][pixel] =
                    ((uint16_t)source[0] << 8) | source[1];
            }

            s_thumbnail_images[row] = (lv_image_dsc_t){
                .header = {.magic = LV_IMAGE_HEADER_MAGIC,
                           .cf = LV_COLOR_FORMAT_RGB565,
                           .w = MEDIA_THUMB_WIDTH,
                           .h = MEDIA_THUMB_HEIGHT,
                           .stride = MEDIA_THUMB_WIDTH * 2},
                .data_size = MEDIA_THUMB_BYTES,
                .data = (const uint8_t *)s_thumbnail_pixels[row]};
            lv_obj_t *image = lv_image_create(item);

            if (NULL == image)
            {
                return false;
            }

            lv_image_set_src(image, &s_thumbnail_images[row]);
        }
        else
        {
            /* 缺失预览使用文字占位，其他媒体仍可浏览和打开。 */
            lv_obj_t *placeholder = CreateLabel(item, "--", MEDIA_THUMB_WIDTH);

            if (NULL == placeholder)
            {
                return false;
            }
        }

        char text[80] = {0};
        bool video = strstr(media->page.names[row], ".AVI") != NULL;
        snprintf(text, sizeof(text), "%.31s\n%s%s", media->page.names[row],
                 video ? "VIDEO" : "PHOTO",
                 media->thumbnail_valid[row] ? "" : " / NO PREVIEW");
        lv_obj_t *name = CreateLabel(item, text, 248);

        if (NULL == name)
        {
            return false;
        }

        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    }

    SelectRow(page, media->page.selected_row);
    return true;
}

/**
 * @authors Codex
 * @brief 同步指定页面并立即渲染，无历史累积或后台SPI事务。
 * @details 状态机掌握逐级返回，lv_menu历史每次重置为当前一页。
 * NULL页面切换清理历史节点，避免长期浏览持续消耗LVGL内存。
 * 输出指针仅在lv_refr_now期间设置，刷新完成即置空。
 * 所有对象位置在渲染前更新，随后滚动到当前选中行。
 * 此接口返回不代表LCD已显示；上层必须提交图像并等待DMA。
 * 不从LVGL事件中访问磁盘，刷新和TF写入保持明确顺序。
 * @param page 非空本模块页。
 * @param title 非空标题。
 * @param footer 非空两行提示。
 * @param pixels 非空完整屏幕缓冲，只借用至本函数返回。
 * @return true渲染完成。
 */
static bool RenderPage(lv_obj_t *page, const char *title, const char *footer,
                       uint8_t *pixels)
{
    lv_label_set_text(s_header, title);
    lv_label_set_text(s_footer, footer);
    lv_menu_set_page(s_menu, NULL);
    lv_menu_set_page(s_menu, page);
    /* 固定外部页眉显示标题，隐藏库内无标题的header以保留四行空间。
     * 所有页面切换都重绘全屏，避免与直接相机画面的旧像素混合。 */
    lv_obj_add_flag(lv_menu_get_main_header(s_menu), LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(page);

    /* 普通菜单超过四行时，必须在新页挂到主区域后再定位焦点。
     * 隐藏页预先滚动不能保证它的新高度已参与布局计算。 */
    for (unsigned row = 0; row < lv_obj_get_child_count(page); ++row)
    {
        lv_obj_t *item = lv_obj_get_child(page, (int32_t)row);

        if (lv_obj_has_state(item, LV_STATE_FOCUSED))
        {
            lv_obj_scroll_to_view(item, LV_ANIM_OFF);
        }
    }

    lv_obj_invalidate(lv_display_get_screen_active(s_display));
    s_output = pixels;
    lv_refr_now(s_display);
    s_output = NULL;
    return true;
}

/**
 * @authors Codex
 * @brief 同步菜单模型，格式化页面只有显示权限，没有文件系统接口。
 * @details 参数检查使用无符号范围判断，也拒绝负数页面值。
 * 未创建的取景/回放页不能误解释为格式化或功能菜单。
 * 所有文本复制进LVGL标签，不保存CameraUi或MediaListView指针。
 * 功能选择由menu_selected表示，格式选择由format_yes单独表示。
 * 页脚将通知和按键提示分两行，避免错误文字覆盖操作提示。
 * 控件创建失败只返回错误，不产生拍照、录制或擦除动作。
 * @param ui 非空只读页面状态。
 * @param media 媒体页模型或NULL，不持有。
 * @param detail 使用状况正文，可为NULL。
 * @param notice 通知，可为NULL。
 * @param pixels 非空完整屏幕输出。
 * @return true有效页面绘制完成，false无效输入或分配失败。
 */
bool LvglMenuRender(const CameraUi *ui, const MediaListView *media,
                    const char *detail, const char *notice, uint8_t *pixels)
{
    if ((NULL == s_display) || (NULL == ui) || (NULL == pixels)
        || ((unsigned)ui->screen >= MENU_PAGE_COUNT)
        || (NULL == s_pages[ui->screen]))
    {
        return false;
    }

    char title[64] = {0};
    char footer[128] = {0};
    const char *controls = "K0:OK K1:DOWN K2:UP HOLD K0:BACK";

    switch (ui->screen)
    {
    case UI_MENU:
    case UI_TF_MENU:
        snprintf(title, sizeof(title), "%s",
                 (UI_MENU == ui->screen) ? "FUNCTIONS" : "TF CARD");
        SelectRow(s_pages[ui->screen], ui->menu_selected);
        break;
    case UI_CARD_STATUS:
        snprintf(title, sizeof(title), "TF CARD USAGE");
        lv_label_set_text(
            s_detail, (NULL != detail) ? detail : "CARD STATUS UNAVAILABLE");
        controls = "K0:BACK  HOLD K0:BACK";
        break;
    case UI_FORMAT_PROMPT:
    case UI_FORMAT_CONFIRM:
    {
        snprintf(title, sizeof(title), "%s",
                 (UI_FORMAT_PROMPT == ui->screen) ? "FORMAT TF CARD"
                                                  : "WARNING: ERASE ALL DATA");
        unsigned stage = (UI_FORMAT_CONFIRM == ui->screen) ? 1 : 0;

        for (unsigned choice = 0; choice < 2; ++choice)
        {
            lv_obj_remove_state(s_format_rows[stage][choice], LV_STATE_FOCUSED);

            if (choice == (ui->format_yes ? 1U : 0U))
            {
                lv_obj_add_state(s_format_rows[stage][choice],
                                 LV_STATE_FOCUSED);
            }
        }

        controls = "K0:OK K1:DOWN K2:UP HOLD K0:CANCEL";
        break;
    }
    case UI_BROWSER:
        if (!UpdateMedia(media))
        {
            return false;
        }

        snprintf(title, sizeof(title), "MEDIA %u/%u",
                 ((NULL != media) && (media->page.file_count > 0))
                     ? media->page.selected_index + 1
                     : 0,
                 (NULL != media) ? media->page.file_count : 0);
        break;
    default:
        /* 无预览/回放菜单页，调用者使用直接相机和媒体显示接口。 */
        return false;
    }

    snprintf(footer, sizeof(footer), "%.39s\n%s",
             (NULL != notice) ? notice : "", controls);
    return RenderPage(s_pages[ui->screen], title, footer, pixels);
}

/**
 * @authors Codex
 * @brief 同步渲染不可交互消息，不经键盘或菜单事件执行任何操作。
 * @details 消息页不含CANCEL/YES选择，正在格式化时不引入新授权。
 * 仅呈现状态，调用者决定何时开始和完成实际格式化。
 * 消息文本复制，允许传入临时诊断字符串。
 * @param message 非空消息，标签复制。
 * @param pixels 非空完整输出缓冲。
 * @return true成功，false未初始化或无效参数。
 */
bool LvglMenuMessage(const char *message, uint8_t *pixels)
{
    if ((NULL == s_display) || (NULL == message) || (NULL == pixels))
    {
        return false;
    }

    lv_label_set_text(s_message_label, message);
    return RenderPage(s_message_page, "TF CARD", "", pixels);
}

/**
 * @authors Codex
 * @brief 撤销显示及全部对象，静态描述符不再被图像缓存引用。
 * @details LVGL负责释放页面树及内部菜单历史，不能重复free子对象。
 * 静态绘制及缩略图数组无需free，但其对象引用必须先撤销。
 * 所有存储的对象指针清空，下一次初始化不能访问已删除对象。
 * 不删除调用者屏幕缓冲，不卸载TF，不修改当前相机状态。
 * @return None；同步渲染和LCD提交结束后调用，不释放调用者内存。
 */
void LvglMenuDeinit(void)
{
    if (NULL != s_display)
    {
        /* display拥有根页和隐藏页面，删除顺序由LVGL统一负责。
         * 之后清除图像缓存，不能让缓存指向已撤销的描述符引用。
         * 其他显示若将来存在，仍保持它们的全局LVGL生命周期。 */
        lv_display_delete(s_display);
        s_display = NULL;
        lv_image_cache_drop(NULL);
    }

    s_menu = NULL;
    s_header = NULL;
    s_footer = NULL;
    s_detail = NULL;
    s_message_page = NULL;
    s_message_label = NULL;
    s_output = NULL;
    memset(s_pages, 0, sizeof(s_pages));
    memset(s_format_rows, 0, sizeof(s_format_rows));
}
