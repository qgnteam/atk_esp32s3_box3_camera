/**
 * @file camera_storage.c
 * @author Codex
 * @brief BOOT/USB 控制拍照及无声 AVI 录像，支持 exFAT/FAT TF 卡。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：启用 exFAT、串口控制与完整文件读回；法律信息：None。
 */
#include "camera_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
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
#include "ff.h"
#include "media_format.h"

/* exFAT 需要 FatFs 全组件使用一致配置，不能只改应用的头文件。
 * 若本地覆盖组件或长文件名配置失效，编译即失败，避免静默降级。 */
_Static_assert(FF_FS_EXFAT == 1, "BOX3 requires exFAT-enabled FatFs");
_Static_assert(FF_USE_LFN != 0, "exFAT requires long filename support");

/* 5 FPS 是写入上限；60 秒自动停止，最大 300 帧，避免 FAT 大文件。
 * 无压缩 QVGA BGR24 每帧 230400 字节，5 FPS 约 1.15 MB/s。
 * 按键稳定 40 ms 后确认，按住 800 ms 以上松开切换录像。
 * 单任务拥有文件与转换缓冲，不跨任务持有 CameraFrame。 */
#define FRAME_PERIOD_US    (200000)
#define RECORD_LIMIT_US    (60000000)
#define RECORD_MAX_FRAMES  (300)
#define DEBOUNCE_US        (40000)
#define LONG_PRESS_US      (800000)
#define PIXEL_BUFFER_BYTES (MEDIA_WIDTH * 3 * 16)
static const char *kTag = "storage";
static sdmmc_card_t *s_card = NULL;
static uint8_t *s_pixels = NULL;
static FILE *s_video = NULL;
static char s_path[64] = {0};
static uint32_t s_frames = 0;
static int64_t s_started = 0;
static int64_t s_next_frame = 0;
static int64_t s_last_frame = 0;
static bool s_button_ready = false;
/* 控制台输出不代表驱动已安装，读取前必须确认初始化成功。 */
static bool s_console_ready = false;
/* 最近发布文件仅供串口读回验证；不接收外部任意路径。 */
static char s_saved_path[64] = {0};

/**
 * @authors Codex
 * @brief 读取本任务持有的视频文件状态，不访问卡或帧内存。
 * @details 文件指针非空表示状态为录制，不代表最近一帧已写入。
 * 状态在创建文件和初始头成功后保持，错误或停止时立即清空。
 * 显示调用发生在存储处理前，标记最多滞后一轮取景。
 * 查询不阻塞、不调用 FAT，也不获取摄像头帧。
 * 不允许另一个任务调用本接口并同时修改录像状态。
 * @return 是否存在正在录制的视频。
 */
bool StorageIsRecording(void)
{
    return NULL != s_video;
}

/**
 * @authors Codex
 * @brief 卡驱动卸载将 CS 改为输入后，恢复高电平避免干扰共用 LCD。
 * @details SDSPI 卸载只删除卡设备，不释放板级 SPI 总线。
 * LCD 设备继续存活，所以错误路径也必须保持卡片未选中。
 * 本函数只能在卡事务结束后调用，不中断正在执行的传输。
 * 高电平写入输出锁存后再切换方向，降低片选毛刺风险。
 * 不修改 SCLK、MOSI、MISO，也不修改 LCD 的 CS47。
 * CS18 是卡槽的物理引脚，不能传入扩展器 XIO 索引。
 * @return None；无卡和错误清理路径调用，不在正常事务期间调用。
 */
static void DeselectCard(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_18,
        .mode = GPIO_MODE_OUTPUT,
    };
    /* 最新修改：ESP-IDF sdspi deinit 会把 CS 设为输入，必须恢复。
     * 输出锁存先拉高，再配置输出，避免低电平毛刺选中卡片。 */
    gpio_set_level(GPIO_NUM_18, 1);
    gpio_config(&config);
}

/**
 * @authors Codex
 * @brief 复用 LCD 的 SPI2 总线挂载 TF，GPIO18 独立片选；不格式化。
 * @details 已挂载时直接返回，不重复创建设备和 FAT 注册。
 * 卡初始化失败清空句柄，调用者不能把失败句柄当作有效卡。
 * 失败后只允许下一次用户操作重试，不在每张取景帧重试。
 * 目录创建失败会先卸载本次卡设备再返回失败。
 * DCIM 使用固定短名称，不依赖长文件名或实时时钟。
 * exFAT 或 FAT32 格式需由用户在电脑准备，程序不擦除已有内容。
 * 启动时调用必须早于 LCD 命令，使卡先切入 SPI 模式。
 * 存储函数只借用 BoardInitSpi 创建的 SPI2 总线。
 * 运行中重新挂载需保证 LCD 上一张 DMA 已完整结束。
 * 无卡时返回失败但不终止 application 的预览循环。
 * 挂载成功不是可写保证，后续每次写入仍检查返回值。
 * @return 是否挂载成功；失败可在下次按键时重试。
 */
static bool MountCard(void)
{
    if (NULL != s_card)
    {
        return true;
    }

#ifdef CONFIG_BOX3_SD_ENABLED
    /* 用户确认 TF 为 GPIO15/16/17/18，与 LCD 共用前三根 SPI 信号。
     * BoardInitSpi 已创建 SPI2，总线不能再次初始化或由存储模块释放。
     * 卡和 LCD 都是 mode 0，设备频率分别配置，由 SPI 驱动切换。
     * 主任务在 LCD DMA 完成后写卡，因此不会交叉借用像素缓冲。 */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = GPIO_NUM_18;
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t result =
        esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &s_card);

    if (ESP_OK != result)
    {
        s_card = NULL;
        DeselectCard();
        ESP_LOGE(kTag, "TF mount failed: %s; card not formatted",
                 esp_err_to_name(result));
        return false;
    }

    if ((mkdir("/sdcard/DCIM", 0775) != 0) && (errno != EEXIST))
    {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_card = NULL;
        DeselectCard();
        return false;
    }

    FATFS *filesystem = NULL;
    DWORD free_clusters = 0;
    FRESULT status = f_getfree("0:", &free_clusters, &filesystem);

    if ((FR_OK == status) && (NULL != filesystem))
    {
        ESP_LOGI(kTag, "TF mounted at /sdcard/DCIM fs=%s free_clusters=%lu",
                 filesystem->fs_type == FS_EXFAT ? "exFAT" : "FAT",
                 (unsigned long)free_clusters);
    }
    else
    {
        ESP_LOGI(kTag, "TF mounted at /sdcard/DCIM fs_type_query=%d", status);
    }
    return true;
#else
    ESP_LOGW(kTag, "TF disabled in menuconfig / BOX3 camera storage");
    return false;
#endif
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
 * @return None；失败删除本次临时文件并记录日志。
 */
static void PublishFile(FILE *file, const char *path, const char *extension,
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
            esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
            s_card = NULL;
            DeselectCard();
        }
    }
}

/**
 * @authors Codex
 * @brief 为完整帧添加索引并以实际平均采样周期完成视频。
 * @details 正常停止只发布包含至少一张完整帧的视频。
 * 零帧录像不生成空 AVI，关闭后按失败文件处理。
 * 索引逐条写出，内存不随录像帧数增长。
 * 任一索引写入失败阻止文件最终发布。
 * fseek 回到文件开头后覆盖同样大小的头，不移动像素块。
 * 周期用整数微秒记录，避免浮点帧率比较。
 * 两帧以上用首末采样时间除以间隔数计算平均周期。
 * 一帧采用目标周期，避免除以零或零长度播放。
 * 停止由按键、时间边界、摄像头恢复或退出预览触发。
 * 本函数不调用 CameraGet，也不等待下一张图像。
 * 在完整写入帧之外发生捕获错误时仍可正常封闭已有帧。
 * @return None；无录像时无操作，不保留借用帧。
 */
void StorageStop(void)
{
    if (NULL == s_video)
    {
        return;
    }

    uint8_t chunk[16] = {0};
    memcpy(chunk, "idx1", 4);
    MediaPut32(chunk + 4, s_frames * 16);
    bool valid = (s_frames > 0) && (fwrite(chunk, 1, 8, s_video) == 8);

    for (uint32_t index = 0; valid && (index < s_frames); ++index)
    {
        /* 索引相对 movi 类型标识开始，首帧块偏移为四字节。
         * 每帧均为独立未压缩关键帧，标记 AVIIF_KEYFRAME。
         * 索引按确定的固定帧大小计算，无需全程保存索引数组。 */
        memcpy(chunk, "00db", 4);
        MediaPut32(chunk + 4, 0x10);
        MediaPut32(chunk + 8, 4 + (index * (MEDIA_FRAME_BYTES + 8)));
        MediaPut32(chunk + 12, MEDIA_FRAME_BYTES);
        valid = fwrite(chunk, 1, sizeof(chunk), s_video) == sizeof(chunk);
    }

    uint32_t period = FRAME_PERIOD_US;

    if (s_frames > 1)
    {
        period = (uint32_t)((s_last_frame - s_started) / (s_frames - 1));
    }

    uint8_t header[MEDIA_AVI_HEADER_BYTES] = {0};
    MediaAviHeader(header, s_frames, period ? period : FRAME_PERIOD_US);
    valid = valid && (fseek(s_video, 0, SEEK_SET) == 0)
            && (fwrite(header, 1, sizeof(header), s_video) == sizeof(header));
    FILE *completed = s_video;
    s_video = NULL;
    ESP_LOGI(kTag, "RECORD_STOP frames=%" PRIu32 " period_us=%" PRIu32,
             s_frames, period);
    PublishFile(completed, s_path, "AVI", valid);
}

/**
 * @authors Codex
 * @brief 出错时丢弃未完成视频，避免部分帧导致索引偏移错误。
 * @details 只在 s_video 有效并且当前写入出错时调用。
 * 部分帧可能已经占据磁盘字节，不能按完整帧数计算索引。
 * 因此不尝试回填正常文件头，也不发布 AVI 扩展名。
 * 清空状态在关闭之前执行，后续显示不会继续提示录制。
 * 文件关闭和卡卸载仍使用共用的错误收尾流程。
 * 失败录像的相机借用帧仍由应用负责归还。
 * @return None
 */
static void AbortVideo(void)
{
    FILE *failed = s_video;
    s_video = NULL;
    PublishFile(failed, s_path, "AVI", false);
}

/**
 * @authors Codex
 * @brief BOOT 短按拍照，长按松开切换录像；过滤抖动和启动时按住。
 * @details 轮询在有效取景帧后执行，按键延迟与帧处理时间有关。
 * 去抖时间和长按时间都采用 esp_timer 的单调微秒计时。
 * 电平变动只更新候选状态，不立即发送按键事件。
 * 候选电平稳定超过阈值后才更新已确认的按下状态。
 * 每次松开仅产生一个事件，长按期间不连续切换录像。
 * 启动时先观察松开，避免按住复位产生意外操作。
 * 由于使用帧后轮询，极短按键可能无法采样，应明显按下再松开。
 * 没有 GPIO 中断，卡写入和文件分配都不在 ISR 中执行。
 * @return 0 无事件，1 拍照，2 切换录像。
 */
static unsigned ButtonEvent(void)
{
    static int previous = 1;
    static int stable = 1;
    static int64_t changed = 0;
    static int64_t pressed = 0;
    static bool armed = false;
    int level = gpio_get_level(GPIO_NUM_0);
    int64_t now = esp_timer_get_time();

    /* 启动时一直按住 BOOT 不产生拍照/录像事件，先等一次松开。
     * 所有静态状态仅由主循环访问，不存在并发读改写。 */
    if (!armed)
    {
        if (1 == level)
        {
            armed = true;
            previous = 1;
            stable = 1;
            changed = now;
        }

        return 0;
    }

    if (level != previous)
    {
        previous = level;
        changed = now;
    }

    if ((level != stable) && ((now - changed) >= DEBOUNCE_US))
    {
        stable = level;

        if (0 == stable)
        {
            pressed = now;
        }
        else if (pressed != 0)
        {
            unsigned event = ((now - pressed) >= LONG_PRESS_US) ? 2 : 1;
            pressed = 0;
            return event;
        }
    }

    return 0;
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
 * @brief 创建转换缓冲和 BOOT 输入；无卡时仍允许取景。
 * @details 必须先由板级创建共用 SPI2，总线所有权不转移。
 * BOOT 配置内部上拉，使用低有效输入，保留硬件下载功能。
 * 像素缓冲零初始化，成功后存活至重启，不在每帧重复申请。
 * 缓冲分配失败只关闭存储能力，不释放其他模块资源。
 * 初始化只执行一次，不是可重入的重复创建接口。
 * 挂载错误由 MountCard 记录，application 继续初始化显示。
 * 本版没有安全热拔卡检测输入，操作时需保持卡片插好。
 * @return None
 */
void StorageInit(void)
{
    /* 输出控制台在 SDK 启动阶段已注册，注册 VFS 不等于安装读驱动。
     * 驱动安装需要环形缓冲，初始化结果必须先于读接口检查。
     * 已安装时借用现有驱动，不重复申请中断和 RX/TX 缓冲。
     * 新安装成功后 VFS 与直接读取接口共用同一驱动后端。
     * 主任务独占命令读取，不同时启动另一个 console REPL。
     * 设备复位后需重新初始化驱动，不沿用上一轮的句柄状态。
     * USB 驱动失败不关闭摄像头或影响已有板级 SPI 总线。
     * 单字节命令只有在有效取景帧处理时执行，保持帧所有权顺序。
     * 不接主机时零等待读取立即返回，不阻塞物理按键控制。 */
    /* USB 控制台可能仅使用轮询输出，必须显式确保读取驱动已安装。
     * p 拍照、r 录像切换、v 读回校验，与物理按键共用处理路径。
     * 不通过标准输入包装，避免控制台 VFS 连接检查丢弃输入。 */
    s_console_ready = usb_serial_jtag_is_driver_installed();

    if (!s_console_ready)
    {
        usb_serial_jtag_driver_config_t console =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        s_console_ready = usb_serial_jtag_driver_install(&console) == ESP_OK;
    }

    if (s_console_ready)
    {
        usb_serial_jtag_vfs_use_driver();
    }
    else
    {
        ESP_LOGW(
            kTag,
            "USB input driver unavailable; physical BOOT control retained");
    }
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    s_button_ready = gpio_config(&button) == ESP_OK;

    if (!s_button_ready)
    {
        ESP_LOGE(kTag, "BOOT input initialization failed; storage unavailable");
        return;
    }

    s_pixels = heap_caps_calloc(1, PIXEL_BUFFER_BYTES,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (NULL == s_pixels)
    {
        ESP_LOGE(kTag, "pixel buffer allocation failed; storage unavailable");
        return;
    }

    MountCard();
    ESP_LOGI(
        kTag,
        "BOOT: short release=photo, hold >=800ms then release=record toggle");
}

/**
 * @authors Codex
 * @brief 在有效帧持有期处理按键、BMP 图片和有界录像采样。
 * @details 只有有效 QVGA RGB565 帧可以进入转换和文件写入。
 * 每行最少640字节，最后一行范围不能超出有效帧长度。
 * 无按键且未录像时只执行输入轮询，不产生卡写入。
 * 长按停止分支先返回，避免同一事件立即重开视频。
 * 拍照前封闭已有录像，避免同时打开的文件相互错误清理。
 * 新建文件失败保留预览，下次用户操作可再次尝试。
 * 帧完成后才增加录像计数，部分数据永远不算完整帧。
 * 录像采样时间记录开始写入前的取景处理时间。
 * 写入结束再设置下次采样时间，卡慢时不形成补帧积压。
 * 录制时间和最大帧数双重限制保护32位文件长度。
 * 本函数不直接绘制屏幕，红色状态标记由显示模块处理。
 * 帧归还必须在本函数返回后，不能在写入过程中交回驱动。
 * @param frame 非空有效帧，读取期间不能归还给驱动。
 * @return None；所有文件操作串行，失败不终止预览。
 */
void StorageProcess(const CameraFrame *frame)
{
    if ((NULL == frame) || !frame->valid || (NULL == frame->data)
        || (frame->stride < (MEDIA_WIDTH * 2))
        || (frame->length
            < ((MEDIA_HEIGHT - 1) * frame->stride + (MEDIA_WIDTH * 2)))
        || (NULL == s_pixels) || !s_button_ready)
    {
        return;
    }

    unsigned event = ButtonEvent();
    char command = '\0';

    /* 一帧最多读取一个字节，换行及未知字符不触发动作。
     * 验证命令显式执行，不在用户每次保存后自动扫描整个录像。 */
    if (s_console_ready && (usb_serial_jtag_read_bytes(&command, 1, 0) == 1))
    {
        if (command == 'p')
        {
            event = 1;
        }
        else if (command == 'r')
        {
            event = 2;
        }
        else if (command == 'v')
        {
            VerifySavedFile();
        }
    }

    if ((2 == event) && (NULL != s_video))
    {
        StorageStop();
        return;
    }

    if ((event != 0) && MountCard())
    {
        if (1 == event)
        {
            /* 录像期间拍照先正常完成视频，防止两个文件错误清理相互影响。
             * 照片使用 BMP，GC0308 无原生 JPEG，不引入软件编码开销。
             * 图片和视频使用同一 BGR24 转换，颜色、镜像保持一致。 */
            StorageStop();

            if (NULL == s_card)
            {
                return;
            }

            char photo_path[64] = {0};
            FILE *photo = CreateFile("IMG", "BMP", photo_path);

            if (NULL != photo)
            {
                uint8_t header[54] = {0};
                MediaBmpHeader(header);
                bool valid =
                    (fwrite(header, 1, sizeof(header), photo) == sizeof(header))
                    && WritePixels(photo, frame);
                PublishFile(photo, photo_path, "BMP", valid);
            }
            else
            {
                ESP_LOGE(kTag, "cannot create photo errno=%d", errno);
            }
        }
        else
        {
            s_video = CreateFile("VID", "AVI", s_path);

            if (NULL != s_video)
            {
                uint8_t header[MEDIA_AVI_HEADER_BYTES] = {0};
                MediaAviHeader(header, 0, FRAME_PERIOD_US);

                if (fwrite(header, 1, sizeof(header), s_video)
                    != sizeof(header))
                {
                    AbortVideo();
                    return;
                }

                s_frames = 0;
                s_started = esp_timer_get_time();
                s_next_frame = s_started;
                ESP_LOGI(kTag, "RECORD_START %s max_fps=5 limit_seconds=60",
                         s_path);
            }
            else
            {
                ESP_LOGE(kTag, "cannot create video errno=%d", errno);
            }
        }
    }

    int64_t now = esp_timer_get_time();

    if ((NULL != s_video)
        && (((now - s_started) >= RECORD_LIMIT_US)
            || (s_frames >= RECORD_MAX_FRAMES)))
    {
        StorageStop();
    }

    if ((NULL != s_video) && (now >= s_next_frame))
    {
        uint8_t chunk[8] = {0};
        memcpy(chunk, "00db", 4);
        MediaPut32(chunk + 4, MEDIA_FRAME_BYTES);

        if ((fwrite(chunk, 1, sizeof(chunk), s_video) != sizeof(chunk))
            || !WritePixels(s_video, frame) || (fflush(s_video) != 0))
        {
            AbortVideo();
            return;
        }

        if (0 == s_frames)
        {
            s_started = now;
        }

        ++s_frames;
        s_last_frame = now;
        /* 写卡慢时不连续补帧，平均帧率在最终 AVI 头按实测回填。 */
        s_next_frame = esp_timer_get_time() + FRAME_PERIOD_US;
    }
}
