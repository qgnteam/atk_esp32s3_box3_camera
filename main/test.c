/**
 * @file test.c
 * @author None; Codex (entry integration)
 * @brief ESP-IDF 入口，启动摄像头本地预览。
 * @date 原始创建日期无；修改日期 2026-10-12
 * @copyright None
 * @note 修改日志：将原空入口接入 ApplicationStart；法律信息：None。
 */
#include "application.h"

/**
 * @authors None; Codex (entry integration)
 * @brief 启动预览，外设与缓冲由应用统一管理。
 * @return None
 */
void app_main(void)
{
    ApplicationStart();
}
