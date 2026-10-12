/**
 * @file card_format.c
 * @author Codex
 * @brief 固定exFAT格式参数，真实FatFs实现可在虚拟磁盘验证。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：整卡单卷替换旧分区表；法律信息：None。
 */
#include "card_format.h"
#include <stdint.h>

/**
 * @authors Codex
 * @brief 只认可实际文件系统类型，不将所有FAT视作FAT32。
 * @param type 有效或未知FS_*编号。
 * @return exFAT/FAT32为true，其余均false。
 */
bool CardFilesystemSupported(BYTE type)
{
    return (type == FS_EXFAT) || (type == FS_FAT32);
}

/**
 * @authors Codex
 * @brief 从扇区0创建整卡exFAT，32位工作缓冲长度先检查范围。
 * @param drive 非空已注册磁盘编号。
 * @param work 非空工作缓冲，由调用者提供且保留所有权。
 * @param work_bytes 至少4096字节且不超过UINT32_MAX。
 * @return FatFs结果；参数错误不执行写磁盘。
 */
FRESULT CardFormatExfat(const char *drive, void *work, size_t work_bytes)
{
    if ((NULL == drive) || (NULL == work) || (work_bytes < 4096)
        || (work_bytes > UINT32_MAX))
    {
        return FR_INVALID_PARAMETER;
    }

    /* 最新修改：FM_EXFAT明确选择exFAT，FM_SFD创建整卡单卷。
     * 单卷从扇区0开始，替换原分区表，不继承旧分区大小。
     * 16KiB簇适合本项目连续写入，并与读取端保持标准格式。
     * 此函数只提供擦除原语，授权必须在上层状态机完成。
     * 主机测试使用内存磁盘，绝不指向用户设备节点。 */
    MKFS_PARM parameters = {
        .fmt = FM_EXFAT | FM_SFD, .n_fat = 1, .au_size = 16384};
    return f_mkfs(drive, &parameters, work, work_bytes);
}
