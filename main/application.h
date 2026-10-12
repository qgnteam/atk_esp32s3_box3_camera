/**
 * @file application.h
 * @author Codex
 * @brief 实时预览生命周期接口。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：首版接口；法律信息：None。
 */
#ifndef __APPLICATION_H_
#define __APPLICATION_H_

/**
 * @authors Codex
 * @brief 初始化硬件并进入预览循环；只由 app_main 调用一次。
 * @return None；正常情况下持续运行，致命错误后保留诊断画面。
 */
void ApplicationStart(void);

#endif
