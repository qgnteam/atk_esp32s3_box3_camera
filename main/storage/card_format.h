/**
 * @file card_format.h
 * @author Codex
 * @brief TF文件系统类型检查和经确认后使用的exFAT格式原语。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：提取供虚拟磁盘测试；法律信息：None。
 */
#ifndef __CARD_FORMAT_H_
#define __CARD_FORMAT_H_
#include <stdbool.h>
#include <stddef.h>
#include "ff.h"

/**
 * @authors Codex
 * @brief 判断FatFs检测到的实际类型，拒绝FAT12/FAT16。
 * @param type FatFs的FS_*值，不能凭容量推测。
 * @return true仅表示exFAT/FAT32。
 */
bool CardFilesystemSupported(BYTE type);
/**
 * @authors Codex
 * @brief 整卡建立exFAT卷，调用前必须完成用户双确认并关闭所有文件。
 * @param drive 非空已注册磁盘编号，如0:。
 * @param work 非空至少4096字节内部DMA工作缓冲，不转移所有权。
 * @param work_bytes 工作缓冲容量，字节。
 * @return FatFs状态；调用会擦除卡上全部数据，绝不自动调用。
 */
FRESULT CardFormatExfat(const char *drive, void *work, size_t work_bytes);
#endif
