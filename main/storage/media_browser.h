/**
 * @file media_browser.h
 * @author Codex
 * @brief 固定内存的有序文件列表分页，不访问录音或显示状态。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新增四行列表；法律信息：None。
 */
#ifndef __MEDIA_BROWSER_H_
#define __MEDIA_BROWSER_H_
#include <stdbool.h>

/* 文件名容量含终止符；四行对应320x240屏幕可见内容区。 */
#define MEDIA_BROWSER_ROWS       (4)
#define MEDIA_BROWSER_NAME_BYTES (32)

typedef struct
{
    char names[MEDIA_BROWSER_ROWS][MEDIA_BROWSER_NAME_BYTES];
    unsigned count;          /* 本页实际行数，末页可以不足四行。 */
    unsigned selected_row;   /* 本页从零开始的选中行。 */
    unsigned selected_index; /* 全目录从零开始的有序文件位置。 */
    unsigned file_count;     /* 全目录普通BMP/AVI文件个数，不含TMP。 */
} MediaBrowserPage;

/**
 * @authors Codex
 * @brief 扫描选中文件所在页，只保留附近八个名称，不缓存全目录。
 * @param directory 非空已有目录路径，只读，不转移所有权。
 * @param selected 非空目录中普通BMP/AVI文件名，最多31字符。
 * @param page 非空输出，失败时零初始化，不保留输入指针。
 * @return true表示找到选中项并完成分页；空目录/选中项消失返回false。
 * @note 文件按字典序排序；目录变化可由上层重新选择后重试。
 */
bool MediaBrowserReadPage(const char *directory, const char *selected,
                          MediaBrowserPage *page);
#endif
