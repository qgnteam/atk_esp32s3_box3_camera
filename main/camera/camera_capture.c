/**
 * @file camera_capture.c
 * @author Codex
 * @brief esp_video V4L2 GC0308 原生 RGB565 大端采集。
 * @date 2026-10-12
 * @copyright None
 * @note 修改日志：新增保留缓冲的暂停/恢复及借用帧保护；法律信息：None。
 * @note API 使用依据官方 capture_stream 示例，未复制其实现。
 */
#include "camera_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "boards/atk_box3/board.h"

#define CAMERA_BUFFER_COUNT (3)
static const char *kTag = "camera";
static int s_fd = -1;
static bool s_initialized = false;
static bool s_streaming = false;
/* 成功DQBUF总数用于实板证明菜单停流，不因菜单切换或模式重建清零。 */
static uint32_t s_capture_count = 0;
/* MMAP 只借用组件内存；关闭设备时组件释放，不调用 free。 */
static uint8_t *s_buffers[CAMERA_BUFFER_COUNT] = {NULL};
static size_t s_capacities[CAMERA_BUFFER_COUNT] = {0};
static bool s_borrowed[CAMERA_BUFFER_COUNT] = {false};
static size_t s_stride = 0;
/* 当前采集尺寸用于容量校验，拍照模式为传感器原生 VGA。 */
static unsigned s_width = PREVIEW_WIDTH;
static unsigned s_height = PREVIEW_HEIGHT;

/**
 * @authors Codex
 * @brief 将组件提供的 dequeue 超时转换为 timeval。
 * @param milliseconds 最长等待毫秒数。
 * @return ESP_OK 或设置错误；设备需已打开。
 */
esp_err_t CameraSetTimeout(unsigned milliseconds)
{
    /* 接口使用毫秒，V4L2 扩展 ioctl 使用秒与微秒两个字段。
     * 毫秒余数换算成微秒，避免单位混淆导致超时放大。
     * 设为零用于检查已经完成的帧，不等待下一次曝光。
     * 非零时由组件阻塞等待，应用不轮询 DMA 状态。
     * 设置失败保留错误码，由上层关闭并重建采集。 */
    struct timeval timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_usec = (milliseconds % 1000) * 1000,
    };
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout),
                        ESP_FAIL, kTag, "DQBUF timeout errno=%d", errno);
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 配置外部时钟 DVP 接线，确认传感器格式后建立采集缓冲。
 * @param bus 非空共享总线，生命周期长于摄像头。
 * @return ESP_OK 或首个错误；应用必须对失败调用 CameraStop。
 */
esp_err_t CameraInitMode(i2c_master_bus_handle_t bus, bool photo)
{
    /* 这里只借用总线，不创建另一个占用 GPIO2/3 的 I2C 控制器。
     * 传感器控制和扩展器复位均由同一任务串行执行。
     * NC 表示没有 ESP32 直连脚，不得作为实际 GPIO 编号使用。
     * D0～D7 顺序与排线连接一致，交换顺序会破坏像素字节。
     * 在传感器复位完成前不允许启动 DVP 数据接收。 */
    ESP_RETURN_ON_FALSE(NULL != bus, ESP_ERR_INVALID_ARG, kTag, "null bus");
    ESP_RETURN_ON_ERROR(BoardResetCamera(), kTag, "camera reset");
    esp_video_init_dvp_config_t dvp = {
        .sccb_config = {.init_sccb = false, .i2c_handle = bus, .freq = 100000},
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dvp_pin =
            {
                .data_width = CAM_CTLR_DATA_WIDTH_8,
                .data_io = {7, 8, 9, 10, 11, 12, 4, 5},
                .vsync_io = 6,
                .de_io = 46,
                .pclk_io = 45,
                .xclk_io = -1,
            },
        /* 仅描述历史板级时钟基线，不在 NC 引脚产生时钟。 */
        .xclk_freq = 24000000,
    };
    esp_video_init_config_t config = {.dvp = &dvp};
    ESP_RETURN_ON_ERROR(esp_video_init(&config), kTag, "video init");
    s_initialized = true;
    /* 记录组件初始化成功状态，便于半初始化失败时正确回收。
     * V4L2 文件描述符与组件句柄分开管理，不能互相代替。
     * open 失败也需要 deinit 已创建的 SCCB/传感器对象。
     * 只有所有格式和缓冲检查通过才设置 streaming 标志。
     * 摄像头初始化失败不会删除应用借用的共享 I2C 总线。 */
    s_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, kTag, "open DVP errno=%d", errno);

    /* 最新修改：分配缓冲之前选择原生格式；禁止带借用帧重建。
     * GC0308 最大彩色格式为 VGA，预览仍使用 QVGA。
     * 仅使用驱动公布的格式，不修改传感器寄存器表。 */
    s_width = photo ? 640 : PREVIEW_WIDTH;
    s_height = photo ? 480 : PREVIEW_HEIGHT;
    bool found = false;

    for (unsigned index = 0;; ++index)
    {
        struct v4l2_sensor_format_enum entry = {.index = index};

        if (0 != ioctl(s_fd, VIDIOC_ENUM_SENSOR_FMT, &entry))
        {
            break;
        }

        if ((entry.format.width == s_width) && (entry.format.height == s_height)
            && (entry.format.format == ESP_CAM_SENSOR_PIXFORMAT_RGB565_BE))
        {
            ESP_RETURN_ON_FALSE(
                0 == ioctl(s_fd, VIDIOC_S_SENSOR_FMT, &entry.format), ESP_FAIL,
                kTag, "set sensor format");
            found = true;
            break;
        }
    }

    ESP_RETURN_ON_FALSE(found, ESP_ERR_NOT_SUPPORTED, kTag,
                        "missing capture mode");

    /* Kconfig 固定原生 RGB565 BE；读取格式再次验证，避免误显示 YUV。
     * 同时记录传感器格式的标称 FPS 和 XCLK，区分实测 FPS。 */
    esp_cam_sensor_format_t sensor = {0};
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_G_SENSOR_FMT, &sensor),
                        ESP_FAIL, kTag, "sensor format");
    ESP_LOGI(kTag, "sensor=%s nominal_fps=%d xclk=%d", sensor.name, sensor.fps,
             sensor.xclk);
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    /* 读取当前原生格式，不请求驱动执行额外软件颜色转换。
     * RGB565X 指大端 RGB565，与屏幕的线传输顺序匹配。
     * 按当前模式验证尺寸，避免最大分辨率帧按屏幕尺寸读取。
     * 尺寸和格式验证失败时返回，不创建不匹配的采集缓冲。
     * 每行有效跨度以驱动返回值为准，零值才用原生紧凑布局。 */
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_G_FMT, &format), ESP_FAIL, kTag,
                        "capture format");
    ESP_RETURN_ON_FALSE(
        (s_width == format.fmt.pix.width) && (s_height == format.fmt.pix.height)
            && (V4L2_PIX_FMT_RGB565X == format.fmt.pix.pixelformat),
        ESP_ERR_NOT_SUPPORTED, kTag,
        "unexpected capture dimensions or RGB565 byte order");
    s_stride = format.fmt.pix.bytesperline;

    /* 部分版本的 V4L2 层未填 bytesperline；原生无填充 RGB565 用 640。 */
    if (0 == s_stride)
    {
        s_stride = s_width * 2;
    }

    ESP_RETURN_ON_FALSE(s_stride >= (s_width * 2), ESP_ERR_INVALID_SIZE, kTag,
                        "invalid stride");
    ESP_LOGI(kTag, "format=" V4L2_FMT_STR " width=%lu height=%lu stride=%u",
             V4L2_FMT_STR_ARG(format.fmt.pix.pixelformat),
             (unsigned long)format.fmt.pix.width,
             (unsigned long)format.fmt.pix.height, (unsigned)s_stride);
    struct v4l2_requestbuffers request = {
        /* 三个缓冲允许采集、完成帧排队和显示借用交替进行。
         * MMAP 内存由组件分配，不在应用中额外申请整帧副本。
         * 缓冲数量严格检查，避免后续固定索引访问未创建的条目。
         * 队列中的帧由驱动拥有，dequeue 才把借用责任交给应用。
         * 缓冲全部初始化和排队后才启动传感器输出。 */
        .count = CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_REQBUFS, &request), ESP_FAIL,
                        kTag, "request buffers");
    ESP_RETURN_ON_FALSE(CAMERA_BUFFER_COUNT == request.count,
                        ESP_ERR_INVALID_SIZE, kTag, "unexpected buffer count");

    for (unsigned i = 0; i < CAMERA_BUFFER_COUNT; ++i)
    {
        /* QUERYBUF 获取实际容量和映射偏移，不把索引当字节地址。
         * 映射前检查容量，确保 stride 乘高度在分配范围内。
         * mmap 失败既可能返回 NULL，也可能返回 MAP_FAILED。
         * 保存每个容量，后续每次取帧再次检查有效字节长度。
         * 映射成功后立即排队，不读取未经过采集初始化的像素。 */
        struct v4l2_buffer buffer = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_QUERYBUF, &buffer),
                            ESP_FAIL, kTag, "query buffer");
        ESP_RETURN_ON_FALSE(buffer.length >= (s_stride * s_height),
                            ESP_ERR_INVALID_SIZE, kTag, "short mapping");
        s_buffers[i] = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, s_fd, buffer.m.offset);
        ESP_RETURN_ON_FALSE((NULL != s_buffers[i])
                                && (MAP_FAILED != (void *)s_buffers[i]),
                            ESP_ERR_NO_MEM, kTag, "map buffer");
        s_capacities[i] = buffer.length;
        ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_QBUF, &buffer), ESP_FAIL,
                            kTag, "queue initial buffer");
    }

    ESP_RETURN_ON_ERROR(CameraSetTimeout(1000), kTag, "set timeout");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_STREAMON, &type), ESP_FAIL,
                        kTag, "stream on");
    s_streaming = true;
    ESP_LOGI(kTag, "DVP streaming buffers=3 external XCLK (not measured)");
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 取出驱动完成帧并检查索引、容量、长度和错误标志。
 * @param frame 非空输出；成功即取得归还责任，valid=false 也须归还。
 * @return ESP_OK、超时或驱动错误；不会读取无效像素。
 */
esp_err_t CameraGet(CameraFrame *frame)
{
    /* 每次输出从零初始化，失败时调用者看到的是空借用状态。
     * ioctl 超时没有取得帧，所以无需为该次超时执行归还。
     * 成功取帧只提供只读数据，禁止应用修改采集 DMA 内容。
     * s_borrowed 检测重复取出同一令牌，避免多个引用同时持有。
     * 只有 valid=true 的像素可以提交到显示转换链路。 */
    ESP_RETURN_ON_FALSE(NULL != frame, ESP_ERR_INVALID_ARG, kTag, "null frame");
    memset(frame, 0, sizeof(*frame));
    ESP_RETURN_ON_FALSE(s_streaming, ESP_ERR_INVALID_STATE, kTag,
                        "capture paused");
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (0 != ioctl(s_fd, VIDIOC_DQBUF, &buffer))
    {
        return ((EAGAIN == errno) || (ETIMEDOUT == errno)) ? ESP_ERR_TIMEOUT
                                                           : ESP_FAIL;
    }

    ESP_RETURN_ON_FALSE((buffer.index < CAMERA_BUFFER_COUNT)
                            && (!s_borrowed[buffer.index]),
                        ESP_ERR_INVALID_STATE, kTag, "invalid frame ownership");
    s_borrowed[buffer.index] = true;
    ++s_capture_count;
    frame->data = s_buffers[buffer.index];
    frame->index = buffer.index;
    frame->length = buffer.bytesused;
    frame->stride = s_stride;
    frame->width = s_width;
    frame->height = s_height;
    frame->valid = ((buffer.flags & V4L2_BUF_FLAG_DONE) != 0)
                   && ((buffer.flags & V4L2_BUF_FLAG_ERROR) == 0)
                   && (frame->length >= (s_stride * s_height))
                   && (frame->length <= s_capacities[buffer.index]);
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 校验借用状态后重新排入采集队列，成功时清空帧。
 * @param frame 有效借用帧，数据不得继续引用。
 * @return ESP_OK 或驱动错误；失败时禁止再次读取/显示旧帧。
 */
esp_err_t CameraReturn(CameraFrame *frame)
{
    /* 同一令牌不可归还两次，否则驱动可能覆盖仍在显示的帧。
     * 排队成功之后才清空借用状态，失败不能假装归还已完成。
     * 不保留上次读取的 flags 和 bytesused，重建排队描述符。
     * 归还后应用指针清零，调用者不能继续使用该数据地址。
     * 错误由上层恢复处理，本函数不在队列操作中偷偷重启。 */
    ESP_RETURN_ON_FALSE((NULL != frame) && (NULL != frame->data)
                            && (frame->index < CAMERA_BUFFER_COUNT)
                            && s_borrowed[frame->index],
                        ESP_ERR_INVALID_STATE, kTag, "invalid return");
    struct v4l2_buffer buffer = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = frame->index,
    };
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_QBUF, &buffer), ESP_FAIL, kTag,
                        "return buffer errno=%d", errno);
    s_borrowed[frame->index] = false;
    memset(frame, 0, sizeof(*frame));
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 停止 DMA 后关闭 V4L2 设备，组件回收 MMAP 的实际分配。
 * @return ESP_OK 或错误；停止失败保留所有句柄，避免释放在途数据。
 */
esp_err_t CameraStop(void)
{
    /* 停止采集必须先于释放缓冲，防止 DMA 写入已经释放的内存。
     * STREAMOFF 失败时保留句柄，让调用者保留故障证据。
     * close 回收设备内部帧队列，deinit 回收传感器和 SCCB。
     * 停止后原有映射地址全部失效，借用指针随状态清空。
     * 重建使用同一板级总线，共享扩展器不被此函数释放。 */
    if (s_streaming)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_STREAMOFF, &type), ESP_FAIL,
                            kTag, "stream off");
        s_streaming = false;
    }

    if (s_fd >= 0)
    {
        ESP_RETURN_ON_FALSE(0 == close(s_fd), ESP_FAIL, kTag, "close camera");
        s_fd = -1;
    }

    if (s_initialized)
    {
        ESP_RETURN_ON_ERROR(esp_video_deinit(), kTag, "video deinit");
        s_initialized = false;
    }

    /* 映射指针不再有效，立即清空，不能调用标准 free 或 munmap。 */
    memset(s_buffers, 0, sizeof(s_buffers));
    memset(s_capacities, 0, sizeof(s_capacities));
    memset(s_borrowed, 0, sizeof(s_borrowed));
    s_stride = 0;
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 使用屏幕分辨率初始化预览，保留原调用接口。
 * @param bus 有效共享总线，不转移所有权。
 * @return 初始化结果，失败需调用 CameraStop。
 */
esp_err_t CameraInit(i2c_master_bus_handle_t bus)
{
    return CameraInitMode(bus, false);
}

/**
 * @authors Codex
 * @brief 只在主任务且所有借用帧归还后停流，保留MMAP和I2C对象。
 * @details STREAMOFF由驱动停止传感器/控制器并清空queued/done队列。
 * 保留图像分配减少菜单频繁进出的延迟，不意味着DMA仍写PSRAM。
 * 无外部引用时才允许清空队列，否则归还令牌将失效。
 * 成功后更新软件状态，失败保持原状态以防误释放硬件资源。
 * 已暂停的重复调用不重复STREAMOFF，不执行无意义的驱动操作。
 * @return ESP_OK或所有权/驱动错误；不关闭设备，不释放缓冲。
 */
esp_err_t CameraPause(void)
{
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_ERR_INVALID_STATE, kTag,
                        "camera not open");

    for (unsigned index = 0; index < CAMERA_BUFFER_COUNT; ++index)
    {
        ESP_RETURN_ON_FALSE(!s_borrowed[index], ESP_ERR_INVALID_STATE, kTag,
                            "pause requires all frames returned");
    }

    if (!s_streaming)
    {
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_STREAMOFF, &type), ESP_FAIL,
                        kTag, "pause stream off errno=%d", errno);
    s_streaming = false;
    ESP_LOGI(kTag,
             "CAMERA_PAUSED streaming=0 captures=%lu buffers=3 retained=1",
             (unsigned long)s_capture_count);
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 重新排入全部缓冲后启流，不重复初始化传感器或分配PSRAM。
 * @details esp_video停流会把全部缓冲重置为未排队，不能直接STREAMON。
 * 每个映射缓冲仍由设备拥有，重新排队使用原索引和MMAP类型。
 * 队列或启流失败时保持非采集状态，上层须关闭并有限重建。
 * 不允许在部分失败后重复排队，应先CameraStop撤销残留队列。
 * 已采集的重复调用保持原状态，不制造重复队列项。
 * 默认恢复一秒取帧上限；首次取帧仍经过主循环长度和最新帧检查。
 * @return ESP_OK采集已启动，其他为配置/队列/驱动错误。
 */
esp_err_t CameraResume(void)
{
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_ERR_INVALID_STATE, kTag,
                        "camera not open");

    if (s_streaming)
    {
        return ESP_OK;
    }

    for (unsigned index = 0; index < CAMERA_BUFFER_COUNT; ++index)
    {
        ESP_RETURN_ON_FALSE(!s_borrowed[index] && (NULL != s_buffers[index]),
                            ESP_ERR_INVALID_STATE, kTag,
                            "invalid resume buffer");
        struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                     .memory = V4L2_MEMORY_MMAP,
                                     .index = index};
        ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_QBUF, &buffer), ESP_FAIL,
                            kTag, "resume queue errno=%d", errno);
    }

    ESP_RETURN_ON_ERROR(CameraSetTimeout(1000), kTag, "resume timeout");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(0 == ioctl(s_fd, VIDIOC_STREAMON, &type), ESP_FAIL,
                        kTag, "resume stream on errno=%d", errno);
    s_streaming = true;
    ESP_LOGI(kTag, "CAMERA_RESUMED streaming=1 captures=%lu buffers=3 reused=1",
             (unsigned long)s_capture_count);
    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 返回主任务维护的启流状态，不在另一个任务窥探驱动队列。
 * @return true采集中，false未初始化或已停流。
 */
bool CameraIsStreaming(void)
{
    return s_streaming;
}

/**
 * @authors Codex
 * @brief 返回累计成功DQBUF数，菜单停流验证不依赖画面是否变化。
 * @return 无符号32位次数，可能自然回绕，不用作内存索引。
 */
uint32_t CameraGetCount(void)
{
    return s_capture_count;
}
