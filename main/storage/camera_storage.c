/**
 * @file camera_storage.c
 * @author Codex
 * @brief 三键相机模式、TF格式确认、BMP/带声音AVI及本机浏览。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：2026-10-13加入双确认exFAT格式化和PCM录放；法律信息：None。
 */
#include "camera_storage.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "media_format.h"
#include "card_format.h"
#include "media_browser.h"
#include "media_thumbnail.h"
#include "audio/camera_audio.h"
#include "boards/atk_box3/board.h"
#include "display/lcd_display.h"
#include "ui/camera_keys.h"
#include "ui/camera_ui.h"
#include "ui/media_list.h"

/* 覆盖组件必须对所有FatFs使用者保持相同64位文件长度布局。 */
_Static_assert(FF_FS_EXFAT == 1, "BOX3 requires exFAT-enabled FatFs");
_Static_assert(FF_USE_LFN != 0, "exFAT requires long filename support");

/* QVGA无压缩帧230400字节，录像仍限定60秒/300帧。
 * PCM24kHz/16bit单声道，约48000字节/秒，独立任务持续采样。
 * 索引数组4096条覆盖300视频帧及PCM分块，不跨帧持有采集内存。
 * 格式化只由主任务的物理按键双确认动作触发。
 * UART命令仅用于非破坏性调试，不能触发真实格式化。 */
#define FRAME_PERIOD_US      (200000)
#define RECORD_LIMIT_US      (60000000)
#define RECORD_MAX_FRAMES    (300)
#define INDEX_CAPACITY       (4096)
#define PIXEL_BUFFER_BYTES   (MEDIA_WIDTH * 3 * 16)
#define PLAY_MAX_AUDIO_BYTES (AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_BYTES * 64)
static const char *kTag = "storage";
static sdmmc_card_t *s_card = NULL;
static FATFS *s_filesystem = NULL;
static BYTE s_drive = FF_DRV_NOT_USED;
static char s_drive_path[3] = {0};
static bool s_mounted = false;
static bool s_unsupported = false;
static uint8_t *s_pixels = NULL;
static uint8_t *s_index = NULL;
static FILE *s_video = NULL;
static char s_path[64] = {0};
static char s_saved_path[64] = {0};
static uint32_t s_frames = 0;
static uint32_t s_chunks = 0;
static uint32_t s_audio_bytes = 0;
static uint32_t s_movi_bytes = 0;
static int64_t s_started = 0;
static int64_t s_next_frame = 0;
static bool s_console_ready = false;
static bool s_audio_ready = false;
static bool s_audio_active = false;
static CameraUi s_ui = {0};
static char s_notice[40] = {0};
/* 浏览器只打开本目录下的BMP/AVI，拒绝越界尺寸和超大音轨。
 * 图像缓冲为已经校正方向的RGB565，不重复水平镜像。
 * PCM缓冲直到播放任务退出才释放；不会与录音同时运行。 */
static char s_selected[32] = {0};
static unsigned s_file_count = 0;
/* 菜单拥有四个缩略图和一张完整列表图，退出取景时释放。
 * 缩略图缓存按页内文件名匹配，同页移动只改变高亮，不重读卡。
 * 这些内存不交给音频任务，也不影响相机借用帧所有权。 */
static MediaListView s_browser_view = {0};
static uint8_t *s_browser_pixels = NULL;
static uint8_t *s_thumbnails = NULL;
static char s_thumbnail_names[MEDIA_BROWSER_ROWS][MEDIA_BROWSER_NAME_BYTES] = {
    0};
static void LoadBrowserPage(void);
static void ReleaseBrowser(void);

static FILE *s_play_file = NULL;
static uint8_t *s_view = NULL;
static int16_t *s_play_pcm = NULL;
static uint32_t s_play_offsets[RECORD_MAX_FRAMES] = {0};
static uint32_t s_play_frames = 0;
static uint32_t s_play_period = 0;
static uint32_t s_play_next = 0;
static int64_t s_play_started = 0;
static bool s_play_audio_active = false;

/**
 * @authors Codex
 * @brief 查询当前录制状态，仅主任务访问。
 * @return true表示视频文件打开。
 */
bool StorageIsRecording(void)
{
    return NULL != s_video;
}

/**
 * @authors Codex
 * @brief 从小端四字节读取长度，避免未对齐指针解引用。
 * @param bytes 非空且至少四字节输入。
 * @return 小端32位值。
 */
static uint32_t Get32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
           | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/**
 * @authors Codex
 * @brief 驱动卸载后恢复卡CS18高电平，保留共用SPI总线。
 * @return None；只有主任务且无在途LCD DMA时调用。
 */
static void DeselectCard(void)
{
    gpio_config_t config = {.pin_bit_mask = (1ULL << GPIO_NUM_18),
                            .mode = GPIO_MODE_OUTPUT};
    gpio_set_level(GPIO_NUM_18, 1);
    gpio_config(&config);
}

/**
 * @authors Codex
 * @brief 关闭本模块拥有的卷和卡设备，不删除LCD总线。
 * @details
 * 清理顺序为FatFs卷、VFS、diskio、SDSPI设备、卡结构。
 * 驱动可能把CS改成输入，最后重新设置高电平输出。
 * s_drive为FF_DRV_NOT_USED时不能执行diskio注销。
 * 只释放本模块的卡结构，不释放BoardInitSpi创建的总线。
 * 未挂载的未知格式卡也持有物理句柄，仍需执行设备删除。
 * 调用者必须先结束录音和浏览，不能留下悬空FILE。
 * 状态清零以后下一次显式操作才会再次尝试挂载。
 * 重复调用不重复释放已清空的对象。
 * @return None；调用前所有文件和播放任务必须结束。
 */
static void UnmountCard(void)
{
    ReleaseBrowser();
    /* 最新修改：手动分离物理卡初始化和FatFs挂载，保留未知格式卡。
     * 每个注册步骤有相应撤销，失败不会泄漏占用的卷编号。
     * VFS释放其FATFS对象，应用不得重复free该对象。
     * 卡结构由本模块分配，在删除SDSPI设备后才能释放。 */
    if (NULL != s_filesystem)
    {
        f_mount(NULL, s_drive_path, 0);
        esp_vfs_fat_unregister_path("/sdcard");
        s_filesystem = NULL;
    }

    if (s_drive != FF_DRV_NOT_USED)
    {
        ff_diskio_unregister(s_drive);
        s_drive = FF_DRV_NOT_USED;
    }

    if (NULL != s_card)
    {
        sdspi_host_remove_device(s_card->host.slot);
        free(s_card);
        s_card = NULL;
    }

    s_mounted = false;
    s_unsupported = false;
    DeselectCard();
}

/**
 * @authors Codex
 * @brief 只接受exFAT/FAT32；FAT12/FAT16保留原数据并弹出提示。
 * @details
 * 物理设备句柄与已挂载文件系统使用独立状态，不能相互替代。
 * 卡能应答SPI不等于卷能被FatFs识别，因此初始化分两步。
 * 未初始化的卡结构只在失败分支释放，不能读取其中的slot。
 * 磁盘编号由ff_diskio_get_drive取得，避免硬编码0号卷。
 * s_drive_path中的编号同时用于VFS、挂载、查询和格式化。
 * VFS注册成功后FATFS对象由VFS负责释放。
 * 卷类型来自fs_type，不能根据卡容量或原日志推测FAT32。
 * FatFs可挂载FAT12/FAT16，但本应用明确禁止保存到这些类型。
 * FR_NO_FILESYSTEM进入确认页，不代表用户已经允许擦卡。
 * FR_DISK_ERR等通信错误只记录错误，不显示格式化确认。
 * 取消格式提示后可以再次执行存储操作重新打开提示。
 * 每次重新打开提示都恢复默认取消，不能继承旧YES选择。
 * 缺卡时仍继续初始化LCD和摄像头，保留取景功能。
 * 挂载成功后才创建DCIM，防止在未知格式卡上进行写入。
 * 目录已存在允许继续，其他mkdir错误撤销本次注册。
 * 退出错误路径前恢复TF片选，避免影响同总线LCD。
 * 挂载对象与创建文件由主任务串行访问，录音任务不访问FatFs。
 * 换卡后须重新启动，本模块没有热插拔检测输入。
 * @return true表示可写；未知格式与缺卡均不自动格式化。
 */
static bool MountCard(void)
{
    if (s_mounted)
    {
        return true;
    }

    if (s_unsupported)
    {
        /* 取消后再次拍照/录像或浏览可以重新打开提示，不自动擦除。 */
        s_ui.screen = UI_FORMAT_PROMPT;
        s_ui.format_yes = false;
        s_ui.dirty = true;
        return false;
    }

#ifdef CONFIG_BOX3_SD_ENABLED
    /* 先初始化物理卡，成功后再注册FatFs。
     * SD初始化失败属于无卡/通信问题，不能当成需要格式化。
     * FR_NO_FILESYSTEM表示无法识别文件系统，其他错误只显示失败。
     * 已识别的FAT12/FAT16拒绝存储，但不主动改动其数据。 */
    s_card = calloc(1, sizeof(sdmmc_card_t));

    if (NULL == s_card)
    {
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = GPIO_NUM_18;
    /* 使用公开SDSPI接口，避免依赖SDK私有VFS初始化函数。 */
    sdspi_dev_handle_t handle = 0;
    esp_err_t result = sdspi_host_init();
    bool device_created = false;

    if (ESP_OK == result)
    {
        result = sdspi_host_init_device(&slot, &handle);
        device_created = (ESP_OK == result);
    }

    if (ESP_OK == result)
    {
        host.slot = handle;
        result = sdmmc_card_init(&host, s_card);
    }

    if (ESP_OK != result)
    {
        /* 只有设备创建成功才撤销；失败句柄不能交给remove。 */
        if (device_created)
        {
            sdspi_host_remove_device(handle);
        }
        free(s_card);
        s_card = NULL;
        DeselectCard();
        snprintf(s_notice, sizeof(s_notice), "NO CARD / CARD IO ERROR");
        ESP_LOGE(kTag, "TF init failed: %s; no format offered",
                 esp_err_to_name(result));
        return false;
    }

    if (ESP_OK != ff_diskio_get_drive(&s_drive))
    {
        UnmountCard();
        return false;
    }

    s_drive_path[0] = '0' + s_drive;
    s_drive_path[1] = ':';
    ff_diskio_register_sdmmc(s_drive, s_card);
    esp_vfs_fat_conf_t config = {
        .base_path = "/sdcard", .fat_drive = s_drive_path, .max_files = 3};
    result = esp_vfs_fat_register(&config, &s_filesystem);

    if (ESP_OK != result)
    {
        UnmountCard();
        return false;
    }

    FRESULT status = f_mount(s_filesystem, s_drive_path, 1);
    s_unsupported = (FR_NO_FILESYSTEM == status)
                    || ((FR_OK == status)
                        && !CardFilesystemSupported(s_filesystem->fs_type));

    if (s_unsupported)
    {
        ESP_LOGW(kTag,
                 "TF unsupported result=%u fs_type=%u; confirmation required",
                 status, s_filesystem->fs_type);
        s_ui.screen = UI_FORMAT_PROMPT;
        s_ui.format_yes = false;
        s_ui.dirty = true;
        snprintf(s_notice, sizeof(s_notice), "UNSUPPORTED TF");
        return false;
    }

    if (FR_OK != status)
    {
        ESP_LOGE(kTag, "TF mount IO failed result=%u; no format offered",
                 status);
        UnmountCard();
        snprintf(s_notice, sizeof(s_notice), "CARD IO ERROR");
        return false;
    }

    s_mounted = true;

    if ((mkdir("/sdcard/DCIM", 0775) != 0) && (errno != EEXIST))
    {
        UnmountCard();
        return false;
    }

    DWORD clusters = 0;
    FATFS *filesystem = NULL;
    f_getfree(s_drive_path, &clusters, &filesystem);
    ESP_LOGI(kTag, "TF mounted at /sdcard/DCIM fs=%s free_clusters=%lu",
             (s_filesystem->fs_type == FS_EXFAT) ? "exFAT" : "FAT32",
             (unsigned long)clusters);
    s_notice[0] = '\0';
    return true;
#else
    snprintf(s_notice, sizeof(s_notice), "TF DISABLED");
    return false;
#endif
}

/**
 * @authors Codex
 * @brief 双确认后擦除分区并创建exFAT，其他任何路径不调用此函数。
 * @details
 * 动作来自CameraUiHandle的第二层YES确认，不从挂载失败直接调用。
 * 两层确认都由物理按键完成，串口无法设置确认页选择。
 * 第二层页面明确说明全部文件丢失，并再次默认选择取消。
 * 未识别卡或支持格式的卡都不符合s_unsupported前置条件。
 * 分配工作缓冲失败时不执行任何磁盘擦除。
 * 显示格式化进行中后才调用擦除原语，屏幕DMA已完成。
 * CardFormatExfat以FM_EXFAT固定目标格式，不依赖FatFs自动选择。
 * FM_SFD创建从扇区0开始的整卡单卷，会替换旧分区表。
 * 16KiB分配单位仅影响新卷，不改变已有受支持的卡。
 * 格式化后重新挂载并检查FS_EXFAT，不以调用成功作为唯一依据。
 * 工作缓冲由本函数拥有，每条已分配路径都释放一次。
 * 结束时撤销临时挂载，再通过标准MountCard创建DCIM。
 * 失败显示FORMAT FAILED，不自动重复擦除。
 * 格式化不是掉电事务，中途不得断电或拔卡。
 * @return 是否格式化并重新挂载成功；失败仍保留明确屏幕错误。
 */
static bool FormatCard(void)
{
    if (!s_unsupported || (NULL == s_card) || (NULL == s_filesystem))
    {
        return false;
    }

    /* 只允许明确发现不支持格式的卡进入此路径。
     * 分区与格式化是破坏操作，必须先结束所有文件及播放。
     * 工作用内部DMA内存，避免底层块写入直接借用PSRAM。
     * 以整卡单分区创建exFAT；成功后重新验证真实fs_type。
     * 不尝试格式化无卡、IO故障或已支持格式的卡。 */
    uint8_t *work =
        heap_caps_calloc(1, 4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (NULL == work)
    {
        return false;
    }

    LcdShowText("FORMATTING EXFAT...\nDO NOT REMOVE CARD\nDO NOT POWER OFF");
    ESP_LOGW(kTag, "FORMAT_BEGIN explicit physical double confirmation");
    f_mount(NULL, s_drive_path, 0);
    FRESULT status = CardFormatExfat(s_drive_path, work, 4096);

    free(work);
    work = NULL;

    if (FR_OK == status)
    {
        status = f_mount(s_filesystem, s_drive_path, 1);
    }

    bool success = (FR_OK == status) && (s_filesystem->fs_type == FS_EXFAT);
    ESP_LOGI(kTag, "FORMAT_END result=%u exfat=%d", status, success);
    UnmountCard();

    if (success)
    {
        success = MountCard();
    }

    snprintf(s_notice, sizeof(s_notice), "%s",
             success ? "FORMAT OK" : "FORMAT FAILED");
    s_ui.dirty = true;
    return success;
}
/**
 * @authors Codex
 * @brief 创建唯一临时文件，排除最终文件和历史未完成文件。
 * @details 编号范围为 1 到 99999，到达上限时返回 NULL。
 * 不按日期命名，因为当前项目没有 RTC 校时服务。
 * stat 检查已有最终名称，O_EXCL 检查已有临时名称。
 * 权限参数仅供 VFS 接口使用，不改变卡上的原有文件。
 * fdopen 成功后 FILE 拥有描述符，调用者不得再单独 close。
 * fdopen 失败仍由本函数负责关闭描述符并删除新建 TMP。
 * 打开方式可读写，以便视频停止时回填头部而不重开。
 * 调用期间只有主任务创建相机文件，不需要命名互斥量。
 * stat 失败仅 ENOENT 表示可创建，其他错误不能视为文件不存在。
 * 卡上的其他目录、其他应用文件和历史 TMP 不由此函数删除。
 * @param prefix IMG 或 VID，非空。
 * @param extension 最终扩展名，非空。
 * @param path 非空 64 字节输出，保存临时文件路径。
 * @return 新文件或 NULL；调用者负责关闭和发布或删除。
 */
static FILE *CreateFile(const char *prefix, const char *extension, char *path)
{
    struct stat info = {0};

    for (unsigned sequence = 1; sequence <= 99999; ++sequence)
    {
        /* 文件编号在每次创建时检查磁盘，因此重启不会覆盖旧照片。
         * TMP 先独占创建；最终扩展名仅在写入、同步、关闭成功后出现。
         * 上次意外断电留下的 TMP 保留，编号跳过供人工恢复。
         * 单任务创建，最终路径检查与重命名之间没有应用内竞争。 */
        snprintf(path, 64, "/sdcard/DCIM/%s%05u.%s", prefix, sequence,
                 extension);

        if (stat(path, &info) == 0)
        {
            continue;
        }

        if (errno != ENOENT)
        {
            return NULL;
        }

        snprintf(path, 64, "/sdcard/DCIM/%s%05u.TMP", prefix, sequence);
        int descriptor = open(path, O_CREAT | O_EXCL | O_RDWR, 0664);

        if (descriptor >= 0)
        {
            FILE *file = fdopen(descriptor, "wb+");

            if (NULL == file)
            {
                close(descriptor);
                unlink(path);
            }

            return file;
        }

        if (errno != EEXIST)
        {
            return NULL;
        }
    }

    return NULL;
}

/**
 * @authors Codex
 * @brief 将采集帧按底部优先转换并分块写入，内存不超过 15 KiB。
 * @details 此函数不改变文件位置之外的应用状态。
 * 写入前由 StorageProcess 检查有效帧尺寸和 stride。
 * 文件和帧指针均不可为 NULL，转换缓冲必须已经分配。
 * 相机帧仍属 esp_video，读取结束后由 application 归还。
 * 缓冲位于 PSRAM，标准文件接口会交给 SDSPI 完成块写入。
 * 不把采集 MMAP 指针直接交给 SPI DMA。
 * RGB565 转换保持显示中的水平镜像修正和完整颜色位。
 * BMP 和 AVI 共用底部优先的扫描行，避免格式方向不同。
 * 短写返回 false，调用者不能发布包含部分图像的文件。
 * 本函数不负责 fflush/fsync，调用者在适当文件边界同步。
 * @param file 非空有效文件。
 * @param frame 非空有效 RGB565 帧，调用期间所有权仍在应用。
 * @return 是否完整写入；短写立即停止。
 */
static bool WritePixels(FILE *file, const CameraFrame *frame)
{
    for (unsigned row = 0; row < MEDIA_HEIGHT; row += 16)
    {
        /* 16 行缓冲避免逐行 FAT 写入，同时不再分配整个 BGR24 帧。
         * 输入从最下面一行开始，输出与 BMP 正高度对应。
         * 像素转换不修改摄像头 DMA 内存，可继续用于屏幕显示。
         * 固定高度 240 能被 16 整除，末块不会读出帧边界。 */
        for (unsigned line = 0; line < 16; ++line)
        {
            unsigned source_row = MEDIA_HEIGHT - 1 - row - line;
            MediaConvertRow(frame->data + (source_row * frame->stride),
                            s_pixels + (line * MEDIA_WIDTH * 3));
        }

        if (fwrite(s_pixels, 1, PIXEL_BUFFER_BYTES, file) != PIXEL_BUFFER_BYTES)
        {
            return false;
        }
    }

    return true;
}

/**
 * @authors Codex
 * @brief 同步并关闭临时文件，成功才改为最终扩展名。
 * @details 所有路径都关闭 FILE，返回后指针不得再次使用。
 * valid 记录之前的数据写入结果，不因后续同步成功而重置。
 * fflush 检查标准库缓冲，fsync 请求文件系统完成同步。
 * fclose 也检查错误，因为关闭可能触发最后一次写入。
 * 最终扩展名只有三个字符，与短文件名目录保持兼容。
 * rename 仅在所有数据和关闭步骤成功后尝试。
 * 发布失败仅删除当前创建的 TMP，不删除已存在的最终文件。
 * 错误后卸载卡是为了让下一次操作重新探测设备状态。
 * 调用本函数前必须清空 s_video，防止关闭后状态仍显示录制。
 * 同步并不能提供突然断电时的完整文件系统事务保证。
 * 删除或卸载失败仍可能留下 TMP，重启会跳过其编号。
 * @param file 非空有效文件，返回后关闭。
 * @param path 非空临时路径。
 * @param extension 非空最终扩展名。
 * @param valid 是否所有数据写入成功。
 * @return true仅在同步、关闭及重命名全部成功；失败清理并记录。
 */
static bool PublishFile(FILE *file, const char *path, const char *extension,
                        bool valid)
{
    bool success = valid;

    if ((fflush(file) != 0) || (fsync(fileno(file)) != 0))
    {
        success = false;
    }

    if (fclose(file) != 0)
    {
        success = false;
    }

    char final_path[64] = {0};
    snprintf(final_path, sizeof(final_path), "%s", path);
    char *suffix = strrchr(final_path, '.');

    if (NULL != suffix)
    {
        snprintf(suffix + 1, 4, "%s", extension);
    }
    else
    {
        success = false;
    }

    if (success && (rename(path, final_path) == 0))
    {
        ESP_LOGI(kTag, "SAVED %s", final_path);
        snprintf(s_saved_path, sizeof(s_saved_path), "%s", final_path);
        return true;
    }
    else
    {
        ESP_LOGE(kTag, "save failed: %s errno=%d", path, errno);
        unlink(path);
        /* 写卡失败后卸载，下次用户操作重新探测，避免假定旧卡仍在。
         * 禁止在写入时拔卡；此路径仅提供失败后的有限恢复。
         * 失败文件不以 BMP/AVI 名称发布。 */
        if (NULL != s_card)
        {
            UnmountCard();
        }
    }
    return false;
}

/**
 * @authors Codex
 * @brief 重新打开最近保存文件并读完整内容，输出长度和 CRC 供实机验证。
 * @details 只读取由本应用发布的路径，不允许串口输入任意文件名。
 * 文件打开后从头到尾读取，不写入卡，也不改变录像内容。
 * 必须在录像停止后调用，避免读回长期占用共用总线。
 * CRC 证明完整文件可读，图像内容和播放器兼容仍需进一步检查。
 * 结束时关闭读文件，读错误不能记录为成功。
 * 首段读取包含格式头，同时作为内容的一部分计入 CRC。
 * 累计字节数包含头、视频帧块和末尾索引，不只是像素数据。
 * BMP 声明长度包括全部文件，RIFF 声明长度不含最前八字节。
 * 只验证最近一次成功发布文件，不扫描用户其他文件。
 * 原有 TMP 文件不作为有效结果，也不在校验时自动修复。
 * 首段短读或不支持的文件标识会导致最终 valid=false。
 * fread 结束必须区分正常 EOF 和读卡错误。
 * fclose 失败同样使最终结果无效，不仅检查打开是否成功。
 * CRC 采用 ESP ROM 实现，起始值为零，按各读取块继续累计。
 * 程序不在日志输出完整图像字节，避免占用大量串口带宽。
 * 读取磁盘数据不会修改编号、已保存文件名或照片内容。
 * 读回耗时会进入应用帧处理统计，不代表正常取景传输时间。
 * 长录像读回可能持续数十秒，主任务期间不再取新相机帧。
 * 当前借用帧保持有效，返回后 application 仍会正常归还。
 * @return None；结果通过 FILE_VERIFY 日志输出。
 */
static void VerifySavedFile(void)
{
    if ((NULL != s_video) || (s_saved_path[0] == '\0'))
    {
        ESP_LOGW(
            kTag,
            "FILE_VERIFY unavailable: stop recording and save a file first");
        return;
    }

    FILE *file = fopen(s_saved_path, "rb");

    if (NULL == file)
    {
        ESP_LOGE(kTag, "FILE_VERIFY open failed errno=%d", errno);
        return;
    }

    uint8_t header[MEDIA_AVI_HEADER_BYTES] = {0};
    size_t count = fread(header, 1, sizeof(header), file);
    uint32_t crc = esp_rom_crc32_le(0, header, count);
    uint32_t total = (uint32_t)count;
    /* 格式头长度固定，完整文件长度由 BMP/RIFF 小端字段描述。
     * 读回缓冲复用转换区，此时无写入或相机转换并发。 */
    bool bmp = (header[0] == 'B') && (header[1] == 'M');
    bool avi = memcmp(header, "RIFF", 4) == 0;
    unsigned offset = bmp ? 2 : 4;
    uint32_t expected = (uint32_t)header[offset]
                        | ((uint32_t)header[offset + 1] << 8)
                        | ((uint32_t)header[offset + 2] << 16)
                        | ((uint32_t)header[offset + 3] << 24);
    expected += avi ? 8 : 0;

    while ((count = fread(s_pixels, 1, PIXEL_BUFFER_BYTES, file)) > 0)
    {
        total += (uint32_t)count;
        crc = esp_rom_crc32_le(crc, s_pixels, count);
    }

    bool valid = !ferror(file) && (bmp || avi) && (total == expected);

    if (fclose(file) != 0)
    {
        valid = false;
    }

    ESP_LOGI(kTag,
             "FILE_VERIFY path=%s bytes=%" PRIu32 " expected=%" PRIu32
             " crc=%08" PRIx32 " valid=%d",
             s_saved_path, total, expected, crc, valid);
}

/**
 * @authors Codex
 * @brief 写入一个音视频块的idx1条目，以movi标识为偏移原点。
 * @details
 * 偏移原点为movi四字节标识，首块偏移为4。
 * 音频块长度不固定，不能按视频帧数计算全部偏移。
 * s_movi_bytes只记录块头及块数据，不含LIST和movi本身。
 * 每条索引包含标识、关键帧标记、块偏移、数据字节数。
 * 任何索引容量耗尽会中止整段录像，不发布缺索引文件。
 * 索引缓冲位于PSRAM，只有主任务读写。
 * 长度最大受60秒和300帧约束，保持32位RIFF字段范围。
 * 写块失败后无需回滚索引，因为整段TMP会丢弃。
 * @param tag 非空四字节块标识，00db或01wb。
 * @param bytes 块数据长度，字节。
 * @return 是否索引容量足够；调用前块尚未写入。
 */
static bool AddIndex(const char *tag, uint32_t bytes)
{
    if (s_chunks >= INDEX_CAPACITY)
    {
        return false;
    }

    uint8_t *entry = s_index + (s_chunks * 16);
    memcpy(entry, tag, 4);
    MediaPut32(entry + 4, 0x10);
    MediaPut32(entry + 8, 4 + s_movi_bytes);
    MediaPut32(entry + 12, bytes);
    ++s_chunks;
    s_movi_bytes += 8 + bytes;
    return true;
}

/**
 * @authors Codex
 * @brief 将独立任务已采集PCM写入01wb块，每块最多15KiB。
 * @details
 * 复用像素转换缓冲时没有并发图像转换。
 * CameraAudioRead返回有序小端PCM字节，而非四槽原始TDM数据。
 * 读取和写卡不同时持有音频互斥锁，避免存储阻塞采集。
 * 每次排空后再次读取，直到当时环形缓冲为空。
 * 每个音频块都有自己的索引条目和实际长度。
 * 音轨数据不包括块头，s_audio_bytes只累计PCM有效字节。
 * 偶数字节保持16位采样边界，无需添加RIFF奇数字节填充。
 * fwrite短写或索引失败立即传播，不能静默丢弃声音。
 * 音频溢出标志包括DMA队列和软件环形缓冲两种情况。
 * 停止录音后最后排空仍可调用，缓冲不会在停止时释放。
 * @return 是否完整写入；音频溢出不能发布为成功录像。
 */
static bool WriteAudio(void)
{
    size_t bytes = 0;

    while ((bytes = CameraAudioRead(s_pixels, PIXEL_BUFFER_BYTES)) > 0)
    {
        uint8_t header[8] = {0};
        memcpy(header, "01wb", 4);
        MediaPut32(header + 4, bytes);

        /* 视频和音频索引均按实际块长度累积，不能使用旧固定帧偏移。
         * 音轨保存原始采样率，不因卡速下降而拉伸采样。
         * 字节数总为偶数，所以无额外RIFF填充字节。 */
        if (!AddIndex("01wb", bytes)
            || (fwrite(header, 1, sizeof(header), s_video) != sizeof(header))
            || (fwrite(s_pixels, 1, bytes, s_video) != bytes))
        {
            return false;
        }

        s_audio_bytes += bytes;
    }

    return !CameraAudioFailed();
}

/**
 * @authors Codex
 * @brief 采集/写卡失败时停止录音，关闭并删除本次临时录像。
 * @return None，不发布不完整音视频，不删除历史文件。
 */
static void AbortVideo(void)
{
    if (s_audio_active)
    {
        CameraAudioStats stats = {0};
        CameraAudioRecordStop(&stats);
        s_audio_active = false;
    }

    if (NULL != s_video)
    {
        FILE *failed = s_video;
        s_video = NULL;
        PublishFile(failed, s_path, "AVI", false);
    }

    snprintf(s_notice, sizeof(s_notice), "RECORD FAILED");
    s_ui.dirty = true;
}

/**
 * @authors Codex
 * @brief 先停止采样并排空PCM，再完成双流AVI索引、文件头和同步。
 * @details
 * 停止音频任务后统计不会继续变化，可可靠核对总样本数。
 * PCM实际字节数必须等于统计样本数乘以2。
 * 零视频帧或零音频样本不发布空的成功录像。
 * 停止失败和任一短写都使本段录像无效。
 * idx1条目同时包含视频和音频，顺序与movi块一致。
 * 音频保持24kHz，视频平均周期按PCM时长分配。
 * 视频仍为恒定帧率AVI，不表达逐帧可变采样间隔。
 * RIFF大小依据实际音视频块总长度计算。
 * 回填头部不会移动文件数据，因为初始和最终头长度相同。
 * 回填失败仍关闭FILE并丢弃本次临时文件。
 * s_video在发布前清空，下一次绘制不会继续显示REC。
 * fflush、fsync、fclose、rename全部成功才算已保存。
 * 最长视频约60秒，文件长度远小于32位RIFF上限。
 * 恢复摄像头前调用本函数，保证已有完整帧可以正常封闭。
 * @return None；无录制时无操作，只有主任务调用。
 */
void StorageStop(void)
{
    if (NULL == s_video)
    {
        return;
    }

    CameraAudioStats stats = {0};
    esp_err_t audio_result = CameraAudioRecordStop(&stats);
    s_audio_active = false;
    bool valid = (ESP_OK == audio_result) && WriteAudio() && (s_frames > 0)
                 && (s_audio_bytes > 0)
                 && (s_audio_bytes == (stats.samples * AUDIO_SAMPLE_BYTES));
    uint8_t chunk[8] = {0};
    memcpy(chunk, "idx1", 4);
    MediaPut32(chunk + 4, s_chunks * 16);
    valid = valid && (fwrite(chunk, 1, sizeof(chunk), s_video) == sizeof(chunk))
            && (fwrite(s_index, 16, s_chunks, s_video) == s_chunks);

    /* AVI视频使用恒定平均播放周期，时长匹配实际PCM样本时长。
     * 卡速影响取景帧间隔，首版不逐帧保存可变时间戳。
     * 视频总帧数和音频总样本数分别回填，不凭墙钟伪造音频。
     * 相机帧仍归application，停止操作不释放借用帧。 */
    uint32_t period =
        (s_frames > 0)
            ? (uint32_t)(((uint64_t)s_audio_bytes * 1000000)
                         / (AUDIO_SAMPLE_BYTES * AUDIO_SAMPLE_RATE * s_frames))
            : FRAME_PERIOD_US;
    uint8_t header[MEDIA_AV_HEADER_BYTES] = {0};
    MediaAvHeader(header, s_frames, period ? period : FRAME_PERIOD_US,
                  s_audio_bytes, s_movi_bytes, s_chunks);
    valid = valid && (fseek(s_video, 0, SEEK_SET) == 0)
            && (fwrite(header, 1, sizeof(header), s_video) == sizeof(header));
    FILE *completed = s_video;
    s_video = NULL;
    ESP_LOGI(kTag,
             "RECORD_STOP frames=%" PRIu32 " period_us=%" PRIu32
             " audio_bytes=%" PRIu32 " chunks=%" PRIu32 " audio_valid=%d",
             s_frames, period, s_audio_bytes, s_chunks, valid);
    PublishFile(completed, s_path, "AVI", valid);
    s_ui.dirty = true;
}

/**
 * @authors Codex
 * @brief 创建双流临时录像，音频初始化失败时明确拒绝无声降级。
 * @details
 * 音频初始化失败时保留拍照，但录像明确显示AUDIO NOT READY。
 * 不自动创建无声AVI冒充成功的带声音录像。
 * 临时文件使用原有编号排重规则，重启不会覆盖历史文件。
 * 324字节初始头预留视频和PCM两个流。
 * 采集任务启动成功后才设置录制状态和开始时间。
 * 录制开始的音频数据可以在首帧写卡期间积累。
 * 每段录像重置帧数、块数、音频长度和movi长度。
 * 录音任务不共享FILE，不拥有相机借用帧。
 * 创建失败只有本段TMP受影响，历史文件保持不变。
 * 开始时间使用单调计时器，不依赖系统实时时钟。
 * @return None，创建/采集失败清理本次临时文件。
 */
static void StartVideo(void)
{
    if (!s_audio_ready || !MountCard())
    {
        snprintf(s_notice, sizeof(s_notice), "%s",
                 s_audio_ready ? "CARD NOT READY" : "AUDIO NOT READY");
        s_ui.dirty = true;
        return;
    }

    s_video = CreateFile("VID", "AVI", s_path);

    if (NULL == s_video)
    {
        ESP_LOGE(kTag, "cannot create video errno=%d", errno);
        return;
    }

    s_frames = 0;
    s_chunks = 0;
    s_audio_bytes = 0;
    s_movi_bytes = 0;
    uint8_t header[MEDIA_AV_HEADER_BYTES] = {0};
    MediaAvHeader(header, 0, FRAME_PERIOD_US, 0, 0, 0);

    if ((fwrite(header, 1, sizeof(header), s_video) != sizeof(header))
        || (CameraAudioRecordStart() != ESP_OK))
    {
        AbortVideo();
        return;
    }

    s_audio_active = true;
    s_started = esp_timer_get_time();
    s_next_frame = s_started;
    s_notice[0] = '\0';
    ESP_LOGI(kTag, "RECORD_START %s max_fps=5 limit_seconds=60 audio=PCM24000",
             s_path);
    s_ui.dirty = true;
}

/**
 * @authors Codex
 * @brief 以当前有效相机帧保存BMP；文件方向与取景一致。
 * @details
 * 照片操作先正常结束正在进行的录像。
 * 如果视频错误收尾卸载了卡，拍照会再次检查卡状态。
 * 照片头和图像写入任一步失败都禁止发布最终扩展名。
 * 成功提示使用PublishFile真实结果，包含同步和重命名失败。
 * 相机帧只在本函数调用期间读取，不复制或转移借用令牌。
 * 存储文件不包含取景标题和录像标记。
 * @param frame 非空有效帧，只借用至返回。
 * @return None，保存失败不覆盖已有图片。
 */
static void TakePhoto(const CameraFrame *frame)
{
    StorageStop();

    if (!MountCard())
    {
        s_ui.dirty = true;
        return;
    }

    char path[64] = {0};
    FILE *photo = CreateFile("IMG", "BMP", path);

    if (NULL == photo)
    {
        ESP_LOGE(kTag, "cannot create photo errno=%d", errno);
        return;
    }

    uint8_t header[54] = {0};
    MediaBmpHeader(header);
    bool valid = (fwrite(header, 1, sizeof(header), photo) == sizeof(header))
                 && WritePixels(photo, frame);
    valid = PublishFile(photo, path, "BMP", valid);
    snprintf(s_notice, sizeof(s_notice), "%s",
             valid ? "PHOTO SAVED" : "PHOTO FAILED");
    s_ui.dirty = true;
}

/**
 * @authors Codex
 * @brief 退出浏览时释放菜单图像及缩略图缓存，不释放选中文件名称。
 * @details 所有LCD绘制调用已等待DMA完成，菜单缓冲此时不再被读取。
 * 完整列表图153600字节，四个预览合计13824字节。
 * 释放后立即清空指针和缓存名，下一次进入菜单重新读取。
 * 不关闭照片/录像查看器，该生命周期由CloseViewer单独处理。
 * @return None，仅主任务调用，不删除卡上的媒体文件。
 */
static void ReleaseBrowser(void)
{
    free(s_browser_pixels);
    s_browser_pixels = NULL;
    free(s_thumbnails);
    s_thumbnails = NULL;
    memset(&s_browser_view, 0, sizeof(s_browser_view));
    memset(s_thumbnail_names, 0, sizeof(s_thumbnail_names));
}

/**
 * @authors Codex
 * @brief 获取选中页并加载最多四个预览，保持同页缓存。
 * @details 列表只保留有序页，不把全目录或全部缩略图加载到PSRAM。
 * 每个缓存槽记录文件名，跨页文件变化时才重新生成图像。
 * 失败预览同样记住名称，避免每次移动都反复读取损坏文件。
 * 照片查看或视频回放返回时保留菜单缓存，直接恢复高亮页。
 * 源文件只读，每次缩略图读取都及时关闭文件。
 * 缩略图读取不会启动音频播放、扫描完整录像索引或修改媒体。
 * 缓冲申请失败时释放所有菜单内存，保留取景及存储功能。
 * @return None，预览错误显示占位图，不阻止打开其他列表项。
 */
static void LoadBrowserPage(void)
{
    if ((NULL == s_browser_pixels) || (NULL == s_thumbnails))
    {
        ReleaseBrowser();
        s_browser_pixels = heap_caps_calloc(
            1, MEDIA_LIST_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (NULL == s_browser_pixels)
        {
            snprintf(s_notice, sizeof(s_notice), "PREVIEW MEMORY ERROR");
            return;
        }

        s_thumbnails = heap_caps_calloc(MEDIA_BROWSER_ROWS, MEDIA_THUMB_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (NULL == s_thumbnails)
        {
            ReleaseBrowser();
            snprintf(s_notice, sizeof(s_notice), "PREVIEW MEMORY ERROR");
            return;
        }
    }

    MediaBrowserPage page = {0};
    bool found = MediaBrowserReadPage("/sdcard/DCIM", s_selected, &page);
    s_browser_view.page = page;
    s_browser_view.notice = s_notice;

    if (!found)
    {
        /* 空目录保留原错误；选中项消失提示重试，不读取旧缩略图。 */
        if ((s_file_count > 0) && s_mounted)
        {
            snprintf(s_notice, sizeof(s_notice), "LIST CHANGED - RETRY");
        }

        return;
    }

    s_file_count = page.file_count;

    for (unsigned row = 0; row < page.count; ++row)
    {
        uint8_t *thumbnail = s_thumbnails + row * MEDIA_THUMB_BYTES;
        s_browser_view.thumbnails[row] = thumbnail;

        if (strcmp(s_thumbnail_names[row], page.names[row]) == 0)
        {
            continue;
        }

        char path[64] = {0};
        snprintf(path, sizeof(path), "/sdcard/DCIM/%.31s", page.names[row]);
        bool valid =
            MediaThumbnailRead(path, thumbnail, s_pixels, PIXEL_BUFFER_BYTES);
        s_browser_view.thumbnail_valid[row] = valid;
        memcpy(s_thumbnail_names[row], page.names[row],
               MEDIA_BROWSER_NAME_BYTES);
        ESP_LOGI(kTag, "THUMBNAIL name=%s valid=%d crc=%08" PRIx32,
                 page.names[row], valid,
                 esp_rom_crc32_le(0, thumbnail, MEDIA_THUMB_BYTES));
    }

    ESP_LOGI(kTag, "BROWSER_PAGE first=%u rows=%u selected_row=%u total=%u",
             page.selected_index - page.selected_row + 1, page.count,
             page.selected_row, page.file_count);
}

/**
 * @authors Codex
 * @brief 遍历固定DCIM目录，按文件名选择前后项，不缓存整个目录。
 * @details
 * 遍历时维护最近项和边界项，无需为全目录动态分配数组。
 * 向右选择按字典序大于当前名称的最小项。
 * 向左选择按字典序小于当前名称的最大项。
 * 越过边界时选择另一端，实现循环浏览。
 * 初次进入列表选择最小名称，文件顺序可预测。
 * 同长度五位编号名称的字典序与编号序一致。
 * 文件名容量限制为31字节，保留终止零字节。
 * 长度检查在snprintf之前完成，固定路径不会溢出。
 * 只接受普通文件，目录和未完成TMP不作为媒体。
 * 浏览函数不改写文件内容，不自动清理断电遗留TMP。
 * 文件个数来自本次扫描，不沿用前一张卡的缓存。
 * 空目录或读取失败清空选择，K2不会打开未知路径。
 * @param direction -1向左，1向右，0选择第一项；边界循环。
 * @return None，目录错误/空目录清空选择，不打开任意外部路径。
 */
static void SelectFile(int direction)
{
    s_file_count = 0;
    s_notice[0] = '\0';

    if (!MountCard())
    {
        s_selected[0] = '\0';
        LoadBrowserPage();
        return;
    }

    DIR *directory = opendir("/sdcard/DCIM");

    if (NULL == directory)
    {
        s_selected[0] = '\0';
        snprintf(s_notice, sizeof(s_notice), "DIRECTORY READ ERROR");
        LoadBrowserPage();
        return;
    }

    char nearest[32] = {0};
    char edge[32] = {0};
    struct dirent *entry = NULL;

    while ((entry = readdir(directory)) != NULL)
    {
        size_t length = strlen(entry->d_name);

        /* 只支持有界BMP/AVI名称，不加载TMP或隐藏文件。
         * d_type并非所有FatFs版本都有保证，用stat确认普通文件。
         * 不接收串口提供的路径，名称来自固定目录。
         * 不修改、不删除浏览到的历史文件。 */
        if ((length < 5) || (length >= sizeof(s_selected))
            || (entry->d_name[0] == '.')
            || ((strcmp(entry->d_name + length - 4, ".BMP") != 0)
                && (strcmp(entry->d_name + length - 4, ".AVI") != 0)))
        {
            continue;
        }

        char path[64] = {0};
        struct stat info = {0};
        snprintf(path, sizeof(path), "/sdcard/DCIM/%s", entry->d_name);

        if ((stat(path, &info) != 0) || !S_ISREG(info.st_mode))
        {
            continue;
        }

        ++s_file_count;
        bool left = direction < 0;
        bool before = strcmp(entry->d_name, s_selected) < 0;
        bool after = strcmp(entry->d_name, s_selected) > 0;

        if ((edge[0] == '\0')
            || (left ? (strcmp(entry->d_name, edge) > 0)
                     : (strcmp(entry->d_name, edge) < 0)))
        {
            snprintf(edge, sizeof(edge), "%s", entry->d_name);
        }

        if (((left && before) || (!left && after))
            && ((nearest[0] == '\0')
                || (left ? (strcmp(entry->d_name, nearest) > 0)
                         : (strcmp(entry->d_name, nearest) < 0))))
        {
            snprintf(nearest, sizeof(nearest), "%s", entry->d_name);
        }
    }

    closedir(directory);
    const char *chosen =
        ((direction != 0) && (nearest[0] != '\0')) ? nearest : edge;
    snprintf(s_selected, sizeof(s_selected), "%s", chosen);
    LoadBrowserPage();
    ESP_LOGI(kTag, "BROWSE count=%u selected=%s", s_file_count, s_selected);
    s_ui.dirty = true;
}

/**
 * @authors Codex
 * @brief 停止回放任务后释放PCM及图像，关闭浏览文件。
 * @details
 * 音频任务可能仍持有s_play_pcm，先等待其完成信号。
 * 停止超时不能释放PCM，也不能开启另一个播放任务。
 * 任务已经退出后的播放错误仍允许关闭和释放资源。
 * FILE只在主任务使用，关闭后立即置NULL。
 * 全屏图像和预载PCM分别释放，不能混淆所有权。
 * 清零播放帧数和下次帧号，防止旧状态驱动新文件。
 * 照片浏览不创建PCM任务，因此关闭路径更短。
 * 重复关闭空浏览器不重复free或fclose。
 * @return 是否安全结束；超时保留PCM防止任务访问已释放内存。
 */
static bool CloseViewer(void)
{
    if (s_play_audio_active)
    {
        esp_err_t result = CameraAudioPlaybackStop();

        if (ESP_ERR_TIMEOUT == result)
        {
            return false;
        }

        s_play_audio_active = false;
    }

    if (NULL != s_play_file)
    {
        fclose(s_play_file);
        s_play_file = NULL;
    }

    free(s_view);
    s_view = NULL;
    free(s_play_pcm);
    s_play_pcm = NULL;
    s_play_frames = 0;
    s_play_next = 0;
    return true;
}

/**
 * @authors Codex
 * @brief 从文件读取底部优先BGR24帧，转换成已校正方向的RGB565。
 * @param offset 像素起始字节偏移，已在文件解析时验证。
 * @return 是否完整读取；错误不显示部分图像。
 */
static bool ReadImage(uint32_t offset)
{
    if (fseek(s_play_file, offset, SEEK_SET) != 0)
    {
        return false;
    }

    for (unsigned row = 0; row < MEDIA_HEIGHT; ++row)
    {
        if (fread(s_pixels, 1, MEDIA_WIDTH * 3, s_play_file)
            != (MEDIA_WIDTH * 3))
        {
            return false;
        }

        /* 源文件为BGR24底部优先，输出为LCD的大端RGB565顶部优先。
         * 一次只读取一行，避免再分配230400字节中间帧。
         * 输入颜色量化到5/6位，保证照片和AVI共用同一路径。
         * 不水平翻转，存储文件本身已经与取景方向一致。 */
        uint8_t *destination =
            s_view + ((MEDIA_HEIGHT - 1 - row) * MEDIA_WIDTH * 2);

        for (unsigned column = 0; column < MEDIA_WIDTH; ++column)
        {
            uint16_t color = ((uint16_t)(s_pixels[column * 3 + 2] >> 3) << 11)
                             | ((uint16_t)(s_pixels[column * 3 + 1] >> 2) << 5)
                             | (s_pixels[column * 3] >> 3);
            destination[column * 2] = color >> 8;
            destination[column * 2 + 1] = (uint8_t)color;
        }
    }

    return true;
}

/**
 * @authors Codex
 * @brief 验证本项目AVI头和movi边界，扫描视频偏移并预载PCM。
 * @details
 * 仅支持本项目BGR24 AVI，不尝试解码任意压缩视频。
 * 一流旧文件无PCM，双流新文件必须有完整PCM格式描述。
 * 文件头中的宽高、位深和BI_RGB逐项检查。
 * 实际文件长度独立于RIFF声明，必须二者完全一致。
 * 音轨样本数在乘以2之前检查上限，防止整数溢出。
 * 音轨不超过64秒，避免恶意头声明巨量内存。
 * 实际movi范围由LIST长度限定，不能扫描到文件外。
 * 每个块至少有8字节头，剩余长度不足立即失败。
 * 00db块必须恰好包含一帧230400字节数据。
 * 视频偏移数组最多300项，与录像帧数上限一致。
 * 01wb必须为偶数字节，保持16位PCM对齐。
 * 音频块累计不能超过头声明，读入位置经过边界检查。
 * 所有movi块累计结束必须精确落在LIST结束位置。
 * 未知块标识拒绝打开，避免把无关数据解释为PCM。
 * 不把旧idx1视为可信指针，实际扫描重新建立视频偏移。
 * 读取完毕比较实际帧数和实际PCM字节数。
 * 首帧在音频开始前预读，减少回放开头的读卡延迟。
 * 完整PCM缓冲交给独立任务，主任务读视频不会断开音频供给。
 * PCM内存直到CloseViewer确认任务退出才允许释放。
 * 时间轴以单调计时器开始，取景统计不能作为播放时钟。
 * @param header 非空324字节缓冲，已从文件读取。
 * @param file_bytes 文件实际长度，字节。
 * @return 是否格式有效；仅支持原单视频或本版双流AVI。
 */
static bool LoadAvi(const uint8_t *header, uint32_t file_bytes)
{
    uint32_t streams = Get32(header + 56);
    uint32_t data_start =
        (streams == 2) ? MEDIA_AV_HEADER_BYTES : MEDIA_AVI_HEADER_BYTES;
    uint32_t frames = Get32(header + 48);
    uint32_t period = Get32(header + 32);
    uint32_t movi_length = Get32(header + data_start - 8);
    uint32_t audio_samples = (streams == 2) ? Get32(header + 264) : 0;

    if (audio_samples > PLAY_MAX_AUDIO_BYTES / 2)
    {
        return false;
    }

    uint32_t audio_bytes = audio_samples * 2;

    /* 验证固定格式、尺寸、码率和文件长度，拒绝卡上任意AVI编码。
     * 所有大小用实际文件长度约束，避免整数溢出和越界申请。
     * 音轨预载上限64秒，播放期间由独立任务稳定供给I2S。
     * 使用现有索引不是可信边界，扫描实际movi块确认偏移。 */
    if (((streams != 1) && (streams != 2)) || (frames == 0)
        || (frames > RECORD_MAX_FRAMES) || (period < 1000)
        || (period > 10000000) || (memcmp(header + 108, "vidsDIB ", 8) != 0)
        || (Get32(header + 176) != MEDIA_WIDTH)
        || (Get32(header + 180) != MEDIA_HEIGHT) || (header[186] != 24)
        || (Get32(header + 188) != 0)
        || (memcmp(header + data_start - 4, "movi", 4) != 0)
        || (movi_length < 4) || (movi_length - 4 > file_bytes - data_start)
        || (Get32(header + 4) != file_bytes - 8)
        || (audio_bytes > PLAY_MAX_AUDIO_BYTES))
    {
        return false;
    }

    if (streams == 2)
    {
        if ((memcmp(header + 232, "auds", 4) != 0) || (header[296] != 1)
            || (header[298] != 1) || (Get32(header + 300) != AUDIO_SAMPLE_RATE)
            || (Get32(header + 304) != AUDIO_SAMPLE_RATE * 2)
            || (header[308] != 2) || (header[310] != 16) || (audio_bytes == 0))
        {
            return false;
        }

        s_play_pcm = heap_caps_calloc(1, audio_bytes,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (NULL == s_play_pcm)
        {
            return false;
        }
    }

    uint32_t position = data_start;
    uint32_t end = data_start + movi_length - 4;
    uint32_t pcm_position = 0;
    uint32_t frame_position = 0;

    while (position < end)
    {
        uint8_t chunk[8] = {0};

        if ((end - position < 8)
            || (fseek(s_play_file, position, SEEK_SET) != 0)
            || (fread(chunk, 1, sizeof(chunk), s_play_file) != sizeof(chunk)))
        {
            return false;
        }

        uint32_t bytes = Get32(chunk + 4);

        if ((bytes > (end - position - 8)) || ((bytes & 1) != 0))
        {
            return false;
        }

        if (memcmp(chunk, "00db", 4) == 0)
        {
            if ((bytes != MEDIA_FRAME_BYTES) || (frame_position >= frames))
            {
                return false;
            }

            s_play_offsets[frame_position++] = position + 8;
        }
        else if ((streams == 2) && (memcmp(chunk, "01wb", 4) == 0))
        {
            if ((bytes > audio_bytes - pcm_position)
                || (fread((uint8_t *)s_play_pcm + pcm_position, 1, bytes,
                          s_play_file)
                    != bytes))
            {
                return false;
            }

            pcm_position += bytes;
        }
        else
        {
            return false;
        }

        position += 8 + bytes;
    }

    if ((frame_position != frames) || (pcm_position != audio_bytes))
    {
        return false;
    }

    s_play_frames = frames;
    s_play_period = period;
    s_play_next = 0;
    /* 首帧先读入，避免播放开始后才等待整帧读卡造成起始错位。 */
    if (!ReadImage(s_play_offsets[0]))
    {
        return false;
    }

    if (audio_bytes > 0)
    {
        if (CameraAudioPlaybackStart(s_play_pcm, audio_bytes / 2) != ESP_OK)
        {
            return false;
        }

        s_play_audio_active = true;
    }

    s_play_started = esp_timer_get_time();
    ESP_LOGI(kTag,
             "PLAY_START path=%s frames=%lu period_us=%lu audio_bytes=%lu",
             s_selected, (unsigned long)frames, (unsigned long)period,
             (unsigned long)audio_bytes);
    return true;
}

/**
 * @authors Codex
 * @brief 打开选中文件并验证格式，失败回到列表并显示原因。
 * @details
 * 只有固定DCIM目录扫描到的名称可以打开。
 * 先关闭上一媒体，禁止同时持有两套大PCM缓冲。
 * stat限制最大100MiB文件，防止异常长度驱动无界操作。
 * RGB565全屏缓冲从PSRAM申请并零初始化。
 * BMP只支持54字节头、320x240、24位BI_RGB正高度。
 * BMP实际长度必须为230454，不能按错误尺寸读文件。
 * AVI必须带RIFF和AVI标识，再交给LoadAvi验证。
 * 任一读取、申请或格式验证失败都会回到列表。
 * VIEW_FAIL日志可供串口测试定位具体失败文件。
 * 照片打开后停留，视频按帧周期播放并自动回列表。
 * 显示标题使用文件名，不修改磁盘中的像素。
 * 没有删除文件动作，浏览不会损坏历史媒体。
 * @return None，不影响既有文件，不调用文件写入接口。
 */
static void OpenSelected(void)
{
    if (!CloseViewer() || (s_selected[0] == '\0'))
    {
        return;
    }

    char path[64] = {0};
    struct stat info = {0};
    snprintf(path, sizeof(path), "/sdcard/DCIM/%s", s_selected);
    bool valid = (stat(path, &info) == 0)
                 && (info.st_size >= MEDIA_AVI_HEADER_BYTES)
                 && (info.st_size <= (100 * 1024 * 1024));
    s_play_file = valid ? fopen(path, "rb") : NULL;

    if (NULL == s_play_file)
    {
        snprintf(s_notice, sizeof(s_notice), "OPEN FAILED");
        s_ui.dirty = true;
        return;
    }

    s_view = heap_caps_calloc(1, PREVIEW_FRAME_BYTES,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t header[MEDIA_AV_HEADER_BYTES] = {0};
    valid =
        (NULL != s_view)
        && (fread(header, 1, sizeof(header), s_play_file) == sizeof(header));
    bool bmp = (header[0] == 'B') && (header[1] == 'M');

    if (valid && bmp)
    {
        valid = (Get32(header + 2) == (uint32_t)info.st_size)
                && (Get32(header + 10) == 54) && (Get32(header + 14) == 40)
                && (Get32(header + 18) == MEDIA_WIDTH)
                && (Get32(header + 22) == MEDIA_HEIGHT) && (header[26] == 1)
                && (header[28] == 24) && (Get32(header + 30) == 0)
                && (info.st_size == 54 + MEDIA_FRAME_BYTES) && ReadImage(54);
    }
    else if (valid && (memcmp(header, "RIFF", 4) == 0)
             && (memcmp(header + 8, "AVI ", 4) == 0))
    {
        valid = LoadAvi(header, info.st_size);
    }
    else
    {
        valid = false;
    }

    if (!valid)
    {
        CloseViewer();
        snprintf(s_notice, sizeof(s_notice), "UNSUPPORTED / READ ERROR");
        ESP_LOGE(kTag, "VIEW_FAIL path=%s", path);
    }
    else
    {
        s_ui.screen = bmp ? UI_PHOTO : UI_VIDEO;
        s_notice[0] = '\0';
        ESP_LOGI(kTag, "VIEW_OPEN path=%s type=%s", path, bmp ? "BMP" : "AVI");
    }

    s_ui.dirty = true;
}

/**
 * @authors Codex
 * @brief 将已读数据编码成有界HEX行，供文件和菜单图像导出共用。
 * @param bytes 非空输入，只读取到函数返回，不持有指针。
 * @param size 数据字节数，可跨多行，每行最多120字节。
 * @return 是否完整发送，USB超时不继续发送残缺数据。
 * @note 调用期间主任务暂停取景，独立按键任务仍可采样。
 */
static bool ExportHexBytes(const uint8_t *bytes, size_t size)
{
    static const char kHex[] = "0123456789abcdef";
    char line[242] = {0};
    size_t offset = 0;

    while (offset < size)
    {
        size_t count = size - offset;
        count = (count > 120) ? 120 : count;
        line[0] = '@';

        for (size_t index = 0; index < count; ++index)
        {
            uint8_t value = bytes[offset + index];
            line[index * 2 + 1] = kHex[value >> 4];
            line[index * 2 + 2] = kHex[value & 15];
        }

        line[count * 2 + 1] = '\n';
        size_t length = count * 2 + 2;

        if (usb_serial_jtag_write_bytes(line, length, pdMS_TO_TICKS(1000))
            != length)
        {
            return false;
        }

        offset += count;
    }

    return true;
}

/**
 * @authors Codex
 * @brief 只读导出实际菜单绘制缓冲，主机核对缩略图和高亮位置。
 * @details 仅媒体列表页可导出，照片/录像回放和取景时不导出旧菜单。
 * 数据就是提交给LcdDrawImage的RGB565大端图像，不重新构造假界面。
 * 全图153600字节，CRC用于检查USB导出数据是否完整。
 * 不创建TF文件，不访问音频任务或改变用户选中项。
 * @return None，成功通过MENU_FRAME_END日志标识。
 */
static void ExportBrowserFrame(void)
{
    if ((s_ui.screen != UI_BROWSER) || (NULL == s_browser_pixels))
    {
        ESP_LOGW(kTag, "MENU_FRAME unavailable: open browser first");
        return;
    }

    ESP_LOGI(kTag, "MENU_FRAME_BEGIN bytes=%u crc=%08" PRIx32 " selected=%s",
             MEDIA_LIST_BYTES,
             esp_rom_crc32_le(0, s_browser_pixels, MEDIA_LIST_BYTES),
             s_selected);
    bool valid = ExportHexBytes(s_browser_pixels, MEDIA_LIST_BYTES);
    ESP_LOGI(kTag, "MENU_FRAME_END valid=%d", valid);
}

/**
 * @authors Codex
 * @brief 文件导出供主机独立解析验证，以有界HEX行传输。
 * @details
 * 导出入口不接收主机路径，仅访问本应用最近发布文件。
 * 录制或播放时拒绝导出，避免长时间抢占SPI读写。
 * 数据按120字节编码为242字节以内的HEX行。
 * 行长度适配默认256字节USB TX环形缓冲。
 * 每行以@标识，主机将普通日志与数据分离。
 * USB写入有1秒上限，主机停止接收时不会永久阻塞。
 * 导出结束要求正常EOF，读错误不能输出valid=1。
 * 主机检查导出长度和CRC，再使用独立解码器验收。
 * 导出不进行格式化或磁盘写入，也不改变文件编号。
 * 测试产生的主机文件位于被Git忽略的outputs目录。
 * @return None；只读最近保存文件，录像/回放时拒绝。
 */
static void ExportSavedFile(void)
{
    if ((NULL != s_video) || (NULL != s_play_file) || (s_saved_path[0] == '\0'))
    {
        ESP_LOGW(kTag, "EXPORT unavailable");
        return;
    }

    FILE *file = fopen(s_saved_path, "rb");

    if (NULL == file)
    {
        return;
    }

    struct stat info = {0};
    fstat(fileno(file), &info);
    ESP_LOGI(kTag, "EXPORT_BEGIN path=%s bytes=%ld", s_saved_path,
             (long)info.st_size);
    uint8_t bytes[120] = {0};
    size_t count = 0;

    /* 文件导出与菜单截图共用同一HEX编码，行长不超过USB默认缓冲。 */
    while ((count = fread(bytes, 1, sizeof(bytes), file)) > 0)
    {
        if (!ExportHexBytes(bytes, count))
        {
            break;
        }
    }

    bool valid = !ferror(file) && feof(file);
    fclose(file);
    ESP_LOGI(kTag, "EXPORT_END valid=%d", valid);
}

/**
 * @authors Codex
 * @brief 初始化按键、PCM和USB输入，先检测TF格式再初始化LCD。
 * @param bus 有效共享I2C总线，所有权保留在board。
 * @return None；缺卡允许预览，音频失败禁止启动无声录像。
 */
void StorageInit(i2c_master_bus_handle_t bus)
{
    CameraUiInit(&s_ui, false);
    s_console_ready = usb_serial_jtag_is_driver_installed();

    if (!s_console_ready)
    {
        usb_serial_jtag_driver_config_t console =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        console.tx_buffer_size = 4096;
        console.rx_buffer_size = 1024;
        s_console_ready = usb_serial_jtag_driver_install(&console) == ESP_OK;
    }

    if (s_console_ready)
    {
        usb_serial_jtag_vfs_use_driver();
    }

    s_pixels = heap_caps_calloc(1, PIXEL_BUFFER_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_index = heap_caps_calloc(INDEX_CAPACITY, 16,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if ((NULL == s_pixels) || (NULL == s_index))
    {
        snprintf(s_notice, sizeof(s_notice), "STORAGE MEMORY FAILED");
        return;
    }

    esp_err_t keys = CameraKeysInit();
    ESP_LOGI(kTag, "KEYS init=%s", esp_err_to_name(keys));
    s_audio_ready = CameraAudioInit(bus) == ESP_OK;
    MountCard();
    ESP_LOGI(
        kTag,
        "UI READY K2=capture/confirm K1=mode/up K0=menu/down holdK2=cancel");
}

/**
 * @authors Codex
 * @brief 主任务串行执行状态机动作；串口不能执行真实格式化。
 * @details
 * 只有尺寸完整的有效借用帧可以触发拍照或录像帧写入。
 * 输入事件从独立去抖任务队列取得，不在中断中写卡。
 * 每轮只消费一个事件，保持菜单和用户操作的顺序。
 * 实体按键事件优先于串口调试命令。
 * 调试命令经过同一CameraUiHandle状态机进入模式和浏览页。
 * 格式确认页丢弃全部串口模拟键，不能跨页积累YES。
 * 物理双确认之外的所有动作均无法调用FormatCard。
 * 模式切换和进入菜单先结束录像，以完整文件为浏览对象。
 * 切换媒体先关闭音轨，不能同时开启播放与录音。
 * 录像循环每轮先排空PCM，再判断时间和帧数上限。
 * 每个完整视频帧写入后再次排空PCM，缩短积压时间。
 * 写帧或音频失败都调用AbortVideo，不留下成功扩展名。
 * 卡慢时下一帧从写完后再等200ms，不连续补拍积压帧。
 * 录音采样独立于视频采样，声音不会随低视频帧率降采样。
 * 回放以当前墙钟目标帧号为准，读慢时跳过旧视频帧。
 * 回放结束后等待音频任务退出，再返回媒体列表。
 * 恢复或退出由application调用StorageStop，不能遗留录音文件。
 * 本函数从不归还frame，所有权仍由application统一处理。
 * @param frame 当前有效借用相机帧，所有权仍在application。
 * @return None，错误保留预览及诊断。
 */
void StorageProcess(const CameraFrame *frame)
{
    if ((NULL == frame) || !frame->valid || (NULL == frame->data)
        || (frame->stride < MEDIA_WIDTH * 2)
        || (frame->length
            < ((MEDIA_HEIGHT - 1) * frame->stride + MEDIA_WIDTH * 2))
        || (NULL == s_pixels) || (NULL == s_index))
    {
        return;
    }

    CameraKey key = CameraKeysRead();
    bool physical = key != KEY_NONE;
    char command = '\0';
    CameraUiAction direct = UI_ACTION_NONE;

    /* e/l/n/b模拟K2确认/K1上移/K0下移/K2长按取消，方便验证与物理键同一状态机。
     * p/r保留旧测试入口，仅在正常取景页允许拍照/录像。
     * 即使主机连续发送确认，也不能执行卡格式化。
     * d导出最近保存文件，v读回CRC，u只查询当前UI状态。 */
    if (!physical && s_console_ready
        && (usb_serial_jtag_read_bytes(&command, 1, 0) == 1))
    {
        switch (command)
        {
        case 'e':
            key = KEY_CONFIRM;
            break;
        case 'l':
            key = KEY_UP;
            break;
        case 'n':
            key = KEY_DOWN;
            break;
        case 'b':
            key = KEY_CANCEL;
            break;
        case 'p':
            direct =
                (s_ui.screen == UI_PREVIEW) ? UI_ACTION_PHOTO : UI_ACTION_NONE;
            break;
        case 'r':
            direct =
                (s_ui.screen == UI_PREVIEW) ? UI_ACTION_RECORD : UI_ACTION_NONE;
            break;
        case 'v':
            VerifySavedFile();
            break;
        case 'd':
            ExportSavedFile();
            break;
        case 't':
            ExportBrowserFrame();
            break;
        case 'u':
            ESP_LOGI(kTag,
                     "UI_STATUS screen=%u mode=%s selected=%s mounted=%d "
                     "unsupported=%d yes=%d",
                     s_ui.screen, s_ui.video_mode ? "VIDEO" : "PHOTO",
                     s_selected, s_mounted, s_unsupported, s_ui.format_yes);
            break;
        default:
            break; /* 换行和未知字符无动作。 */
        }
    }

    /* 阻止串口接管格式确认流程，主机测试只检查纯状态机。
     * 必须通过设备实体按键逐页确认，不能混合串口YES和实体K2。
     * 当前卡已为FAT32/exFAT时根本不会进入这两个页面。 */
    if (!physical
        && ((s_ui.screen == UI_FORMAT_PROMPT)
            || (s_ui.screen == UI_FORMAT_CONFIRM)))
    {
        key = KEY_NONE;
        direct = UI_ACTION_NONE;
    }

    CameraUiAction action =
        (direct != UI_ACTION_NONE) ? direct : CameraUiHandle(&s_ui, key);

    if ((action != UI_ACTION_NONE) && (action != UI_ACTION_PHOTO)
        && (action != UI_ACTION_RECORD))
    {
        StorageStop();
    }

    if ((action == UI_ACTION_LIST) || (action == UI_ACTION_PREVIOUS)
        || (action == UI_ACTION_NEXT) || (action == UI_ACTION_CLOSE))
    {
        if (!CloseViewer())
        {
            return;
        }
    }

    switch (action)
    {
    case UI_ACTION_PHOTO:
        TakePhoto(frame);
        break;
    case UI_ACTION_RECORD:
        if (NULL != s_video)
        {
            StorageStop();
        }
        else
        {
            StartVideo();
        }
        break;
    case UI_ACTION_LIST:
        SelectFile(0);
        break;
    case UI_ACTION_PREVIOUS:
        SelectFile(-1);
        break;
    case UI_ACTION_NEXT:
        SelectFile(1);
        break;
    case UI_ACTION_OPEN:
        OpenSelected();
        break;
    case UI_ACTION_FORMAT:
        if (physical)
        {
            CloseViewer();
            FormatCard();
        }
        break;
    case UI_ACTION_STOP:
    case UI_ACTION_CLOSE:
    case UI_ACTION_NONE:
    default:
        break; /* 未知动作无磁盘副作用。 */
    }

    if ((action == UI_ACTION_CLOSE) && (s_ui.screen == UI_PREVIEW))
    {
        ReleaseBrowser();
    }

    int64_t now = esp_timer_get_time();

    if (NULL != s_video)
    {
        if (CameraAudioFailed() || !WriteAudio())
        {
            AbortVideo();
            return;
        }

        if (((now - s_started) >= RECORD_LIMIT_US)
            || (s_frames >= RECORD_MAX_FRAMES))
        {
            StorageStop();
            return;
        }

        if (now >= s_next_frame)
        {
            uint8_t chunk[8] = {0};
            memcpy(chunk, "00db", 4);
            MediaPut32(chunk + 4, MEDIA_FRAME_BYTES);

            if (!AddIndex("00db", MEDIA_FRAME_BYTES)
                || (fwrite(chunk, 1, sizeof(chunk), s_video) != sizeof(chunk))
                || !WritePixels(s_video, frame) || !WriteAudio()
                || (fflush(s_video) != 0))
            {
                AbortVideo();
                return;
            }

            ++s_frames;
            s_next_frame = esp_timer_get_time() + FRAME_PERIOD_US;
        }
    }

    /* 视频回放时间由独立PCM任务和同一单调时钟决定。
     * 读卡过慢时跳过已错过的视频帧，保持声音连续且不无限追赶。
     * 完成后关闭音频并回到列表，照片则停留直到用户确认/返回。 */
    if ((s_ui.screen == UI_VIDEO) && (s_play_frames > 0))
    {
        uint32_t target = (uint32_t)((now - s_play_started) / s_play_period);

        if (target >= s_play_frames)
        {
            ESP_LOGI(kTag, "PLAY_STOP complete");
            CloseViewer();
            s_ui.screen = UI_BROWSER;
            s_ui.dirty = true;
        }
        else if (target >= s_play_next)
        {
            if (!ReadImage(s_play_offsets[target]))
            {
                CloseViewer();
                s_ui.screen = UI_BROWSER;
                snprintf(s_notice, sizeof(s_notice), "VIDEO READ FAILED");
            }
            else
            {
                s_play_next = target + 1;
            }

            s_ui.dirty = true;
        }
    }
}

/**
 * @authors Codex
 * @brief 根据UI状态绘制预览、图像或菜单；仅在主任务调用。
 * @details
 * 正常预览每帧绘制，模式和通知只叠加到LCD条缓冲。
 * 格式提示、确认和列表只在dirty时刷新。
 * 照片画面在打开和切换时重绘，不重复读取整张图片。
 * 回放帧读取完成后设置dirty，避免显示半帧。
 * 标题长度受限，超过屏幕宽度的字符被裁剪。
 * 所有绘制接口等待最后DMA完成后返回。
 * 绘制错误传播给application，由应用停止外设访问。
 * 默认取消用选择标记明确显示，YES不是预选项。
 * 格式确认页说明清空全部文件，而非普通保存确认。
 * UI字体为独立5x7英文点阵，不增加大字体库依赖。
 * @param frame 非空有效帧，不转移所有权。
 * @return ESP_OK或LCD错误，菜单未变化时无需刷屏。
 */
esp_err_t StorageDraw(const CameraFrame *frame)
{
    char text[320] = {0};

    if (s_ui.screen == UI_PREVIEW)
    {
        char caption[40] = {0};
        snprintf(caption, sizeof(caption), "%s%s %.26s",
                 s_ui.video_mode ? "VIDEO" : "PHOTO",
                 StorageIsRecording() ? " REC" : "", s_notice);
        LcdSetCaption(caption);
        return LcdDrawFrame(frame->data, frame->stride, StorageIsRecording());
    }

    if (!s_ui.dirty)
    {
        return ESP_OK;
    }

    s_ui.dirty = false;

    if ((s_ui.screen == UI_PHOTO) || (s_ui.screen == UI_VIDEO))
    {
        LcdSetCaption(s_selected);
        return (NULL != s_view) ? LcdDrawImage(s_view) : ESP_ERR_INVALID_STATE;
    }

    if (s_ui.screen == UI_FORMAT_PROMPT)
    {
        snprintf(text, sizeof(text),
                 "TF FORMAT UNSUPPORTED\nNEED EXFAT OR FAT32\nFORMAT AS "
                 "EXFAT?\n\n%s CANCEL   %s YES\nK1:UP "
                 "K0:DOWN\nK2:CONFIRM\nHOLD K2:CANCEL",
                 s_ui.format_yes ? " " : ">", s_ui.format_yes ? ">" : " ");
    }
    else if (s_ui.screen == UI_FORMAT_CONFIRM)
    {
        snprintf(text, sizeof(text),
                 "WARNING: ERASE ALL DATA\nALL FILES WILL BE LOST\nFORMAT "
                 "EXFAT NOW?\n\n%s CANCEL   %s YES\nK1:UP "
                 "K0:DOWN\nK2:CONFIRM\nHOLD K2:CANCEL",
                 s_ui.format_yes ? " " : ">", s_ui.format_yes ? ">" : " ");
    }
    else
    {
        if (NULL == s_browser_pixels)
        {
            return LcdShowText(
                "MEDIA LIST\nPREVIEW MEMORY ERROR\nHOLD K2: CANCEL");
        }

        s_browser_view.notice = s_notice;
        MediaListRender(&s_browser_view, s_browser_pixels);
        LcdSetCaption("");
        return LcdDrawImage(s_browser_pixels);
    }

    return LcdShowText(text);
}
