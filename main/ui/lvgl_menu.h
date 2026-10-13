/**
 * @file lvgl_menu.h
 * @author Codex
 * @brief LVGL 原生多级菜单及媒体列表的同步 RGB565 渲染接口。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：迁移至 LVGL 9.3 lv_menu；法律信息：项目接口无额外许可限制。
 */
#ifndef __LVGL_MENU_H_
#define __LVGL_MENU_H_

#include <stdbool.h>
#include <stdint.h>
#include "camera_ui.h"
#include "media_list.h"

/**
 * @authors Codex
 * @brief 创建单任务 LVGL 显示和菜单页面，不操作 LCD 或磁盘。
 * @param tick 非空毫秒单调时钟回调，初始化后必须保持有效。
 * @return true 成功；失败不允许绘制，调用 Deinit 撤销部分资源。
 * @note 只由相机主任务调用，禁止输入/I2S任务直接访问 LVGL。
 */
bool LvglMenuInit(uint32_t (*tick)(void));

/**
 * @authors Codex
 * @brief 将菜单状态同步至 lv_menu，完成整幅大端 RGB565 绘制。
 * @param ui 非空只读状态，支持菜单、使用状况、确认及媒体列表页。
 * @param media 媒体列表页的借用模型，可为NULL以显示内存错误。
 * @param detail 使用状况页的非空正文，不含操作提示；其他页可为NULL。
 * @param notice 可为NULL的通知字符串，复制进标签后不再持有。
 * @param pixels 非空至少 MEDIA_LIST_BYTES 字节，调用者拥有至返回。
 * @return true 完成绘制；参数或页面无效返回false，不操作磁盘。
 * @note 只在页面变脏时调用；返回后可通过现有同步 LCD 接口显示。
 */
bool LvglMenuRender(const CameraUi *ui, const MediaListView *media,
                    const char *detail, const char *notice, uint8_t *pixels);

/**
 * @authors Codex
 * @brief 显示格式化进行中等不可交互消息，沿用 LVGL 标签和显示。
 * @param message 非空消息，复制后不持有。
 * @param pixels 非空完整屏幕输出缓冲。
 * @return true 完成同步渲染，false 表示无效参数或未初始化。
 */
bool LvglMenuMessage(const char *message, uint8_t *pixels);

/**
 * @authors Codex
 * @brief 释放本模块 LVGL 显示及全部对象，清空借用输出指针。
 * @return None；须在最后一次同步渲染返回后调用，可重复调用。
 * @note 不调用全局 lv_deinit，不影响未来可能添加的其他显示。
 */
void LvglMenuDeinit(void);
#endif
