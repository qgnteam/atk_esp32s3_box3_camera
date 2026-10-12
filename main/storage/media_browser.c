/**
 * @file media_browser.c
 * @author Codex
 * @brief 以选中项为界保留前后候选，目录规模不增加分页内存。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：新列表分页算法；法律信息：None。
 */
#include "media_browser.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/**
 * @authors Codex
 * @brief 在四项候选中按字典序插入，保留最大的或最小的四项。
 * @param names 非空四项名称数组，调用者拥有。
 * @param count 非空当前项数，不能超过四。
 * @param name 非空已检查长度的名称。
 * @param keep_highest true保留最大四项，false保留最小四项。
 * @return None，所有已保留项按升序排列。
 */
static void InsertCandidate(char names[][MEDIA_BROWSER_NAME_BYTES],
                            unsigned *count, const char *name,
                            bool keep_highest)
{
    if (*count == MEDIA_BROWSER_ROWS)
    {
        if (keep_highest)
        {
            if (strcmp(name, names[0]) <= 0)
            {
                return;
            }

            /* 删除最小项后左移，后续按位置插入，不越过数组边界。 */
            memmove(names[0], names[1],
                    (MEDIA_BROWSER_ROWS - 1) * MEDIA_BROWSER_NAME_BYTES);
        }
        else if (strcmp(name, names[*count - 1]) >= 0)
        {
            return;
        }

        --(*count);
    }

    unsigned position = 0;

    while ((position < *count) && (strcmp(names[position], name) < 0))
    {
        ++position;
    }

    /* 向右移动为待插入项腾出空间；循环变量只在for递减表达式修改。 */
    for (unsigned item = *count; item > position; --item)
    {
        memcpy(names[item], names[item - 1], MEDIA_BROWSER_NAME_BYTES);
    }

    snprintf(names[position], MEDIA_BROWSER_NAME_BYTES, "%.31s", name);
    ++(*count);
}

/**
 * @authors Codex
 * @brief 只读扫描并生成选中页，列表范围与旧浏览器一致。
 * @details 每次只保留选中项前四项和后四项，工作内存与目录大小无关。
 * 全目录位置来自实际普通文件计数，不依赖FatFs的readdir顺序。
 * 四行一页，选中位置的余数决定本页高亮行，跨页自然滚动。
 * 目录读取失败时不把部分扫描结果作为有效列表。
 * 隐藏文件、TMP、子目录和超过31字符的名称均不显示。
 * 源目录只读取，不创建、删除或改名任何文件。
 * @param directory 非空目录路径。
 * @param selected 非空已选名称，必须在本次扫描中找到。
 * @param page 非空输出，由调用者持有。
 * @return true表示完整列表页可用；false输出保持零初始化。
 */
bool MediaBrowserReadPage(const char *directory, const char *selected,
                          MediaBrowserPage *page)
{
    if (NULL == page)
    {
        return false;
    }

    memset(page, 0, sizeof(*page));

    if ((NULL == directory) || (NULL == selected) || (selected[0] == '\0'))
    {
        return false;
    }

    DIR *folder = opendir(directory);

    if (NULL == folder)
    {
        return false;
    }

    char before[MEDIA_BROWSER_ROWS][MEDIA_BROWSER_NAME_BYTES] = {0};
    char after[MEDIA_BROWSER_ROWS][MEDIA_BROWSER_NAME_BYTES] = {0};
    unsigned before_count = 0;
    unsigned after_count = 0;
    unsigned rank = 0;
    unsigned total = 0;
    bool found = false;
    struct dirent *entry = NULL;

    bool read_error = false;

    while (true)
    {
        errno = 0;
        entry = readdir(folder);

        if (NULL == entry)
        {
            read_error = (errno != 0);
            break;
        }
        size_t length = strlen(entry->d_name);

        if ((length < 5) || (length >= MEDIA_BROWSER_NAME_BYTES)
            || (entry->d_name[0] == '.')
            || ((strcmp(entry->d_name + length - 4, ".BMP") != 0)
                && (strcmp(entry->d_name + length - 4, ".AVI") != 0)))
        {
            continue;
        }

        /* 组合路径检查截断，使用stat而非不可靠的dirent类型提示。 */
        char path[512] = {0};
        struct stat info = {0};
        int bytes =
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

        if ((bytes < 0) || ((size_t)bytes >= sizeof(path))
            || (stat(path, &info) != 0) || !S_ISREG(info.st_mode))
        {
            continue;
        }

        ++total;
        int compare = strcmp(entry->d_name, selected);

        if (compare <= 0)
        {
            rank += (compare < 0) ? 1 : 0;
            found = found || (compare == 0);
            InsertCandidate(before, &before_count, entry->d_name, true);
        }
        else
        {
            InsertCandidate(after, &after_count, entry->d_name, false);
        }
    }

    bool closed = (closedir(folder) == 0);

    if (!found || !closed || read_error)
    {
        return false;
    }

    /* 从前候选末尾取本页所需前缀，再补充后候选。
     * rank小于四时前候选不足四项，但选中行仍保证前缀存在。
     * 末页没有足够后续文件时保留较少行，不显示伪造的空文件。 */
    page->file_count = total;
    page->selected_index = rank;
    page->selected_row = rank % MEDIA_BROWSER_ROWS;
    unsigned prefix = page->selected_row + 1;

    for (unsigned item = 0; item < prefix; ++item)
    {
        memcpy(page->names[item], before[before_count - prefix + item],
               MEDIA_BROWSER_NAME_BYTES);
    }

    page->count = prefix;

    for (unsigned item = 0;
         (item < after_count) && (page->count < MEDIA_BROWSER_ROWS); ++item)
    {
        memcpy(page->names[page->count], after[item], MEDIA_BROWSER_NAME_BYTES);
        ++page->count;
    }

    return true;
}
