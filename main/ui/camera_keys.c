/**
 * @file camera_keys.c
 * @author Codex
 * @brief 采集三键原始事件，40ms去抖与K0/K2两秒长按。
 * @date 2026-10-13
 * @copyright None
 * @note 修改日志：支持菜单K0长按，保留K2取景长按；法律信息：None。
 */
#include "camera_keys.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "boards/atk_box3/board.h"

/* 事件队列只传值，任务不访问存储文件或摄像头帧。 */
static QueueHandle_t s_events = NULL;
static const char *kTag = "keys";

/**
 * @authors Codex
 * @brief 连续采样，I2C读取失败不转换为按键事件。
 * @details
 * 上电100ms后读取两键松开电平，兼容板上不同有效极性。
 * 初始化时必须松开K1/K2，避免把按住电平校准为松开。
 * K0固定低有效，保留硬件下载模式用途。
 * AW9523B原始输入寄存器为0x00/0x01，与方向寄存器不同。
 * 读取失败不更新候选或稳定值，避免通信错误变成确认。
 * 各键有独立的最近变化时间，防抖不互相影响。
 * 40ms阈值基于微秒单调时钟，不依赖RTOS周期精确度。
 * K0/K2按住达到2秒产生独立长按，短按在松开时产生。
 * K1松开发出上移；主任务按页面映射K0/K2，没有长按重复。
 * 队列元素仅为枚举，不引用任务局部变量或相机内存。
 * 输入任务优先级5，文件操作阻塞时仍可收集松开事件。
 * 队列满时记录丢事件，不把旧事件替换为新的确认。
 * 存储任务收到事件才执行操作，输入任务不能擦卡或写文件。
 * 短按响应由队列保证，不受单帧数百毫秒写卡时间直接限制。
 * @param context 未使用。
 * @return None，任务存活至重启。
 */
static void KeyTask(void *context)
{
    (void)context;
    /* 各键独立过滤状态，采样不受文件操作和显示耗时阻塞。
     * 短按和上下键在松开时入队，K0/K2长按在达到2秒时入队。
     * 长按发出后抬起不补发确认，避免返回列表后意外打开文件。 */
    int idle[3] = {1, 0, 1};
    CameraKeyFilter filters[3] = {0};
    /* 原始输入顺序为K0/K1/K2，过滤器按物理编号映射下/上/确认。
     * K0/K2各自携带长按身份，菜单换键由主任务处理。 */
    uint16_t levels = 0;
    vTaskDelay(pdMS_TO_TICKS(100));

    if (ESP_OK == BoardReadKeys(&levels))
    {
        idle[1] = levels & 1;
        idle[2] = (levels >> 1) & 1;
    }

    for (unsigned key = 0; key < 3; ++key)
    {
        CameraKeyFilterInit(&filters[key], idle[key], (CameraButton)key);
    }

    ESP_LOGI(kTag, "READY K0=GPIO0 K1=XIO0 K2=XIO1 idle=%d,%d,%d", idle[0],
             idle[1], idle[2]);

    while (true)
    {
        if (ESP_OK != BoardReadKeys(&levels))
        {
            /* 通信故障期间保持原状态，不能凭未知电平触发格式化。 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int actual[3] = {gpio_get_level(GPIO_NUM_0), levels & 1,
                         (levels >> 1) & 1};
        int64_t now = esp_timer_get_time();

        for (unsigned key = 0; key < 3; ++key)
        {
            CameraKey event =
                CameraKeyFilterSample(&filters[key], actual[key], now);

            if (event == KEY_NONE)
            {
                continue;
            }

            /* 即时长按和松开短按使用同一队列；满队列不转成确认事件。
             * 过滤器已消耗长按，队列失败也不会在抬起时误拍照。 */
            if (pdTRUE != xQueueSend(s_events, &event, 0))
            {
                ESP_LOGW(kTag, "event queue full");
            }
            else
            {
                ESP_LOGI(kTag, "EVENT key=%u", (unsigned)event);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @authors Codex
 * @brief 初始化直连K0输入，创建16项事件队列和独立采样任务。
 * @return ESP_OK或错误；失败不保留可用事件接口。
 */
esp_err_t CameraKeysInit(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&config);

    if (ESP_OK != result)
    {
        return result;
    }

    s_events = xQueueCreate(16, sizeof(CameraKey));

    if (NULL == s_events)
    {
        return ESP_ERR_NO_MEM;
    }

    if (pdPASS != xTaskCreate(KeyTask, "camera_keys", 3072, NULL, 5, NULL))
    {
        vQueueDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @authors Codex
 * @brief 消费事件，不阻塞预览；任务队列保证同步。
 * @return KEY_NONE、一次短按或按住2秒时的长按事件。
 */
CameraKey CameraKeysRead(void)
{
    CameraKey event = KEY_NONE;

    if (NULL != s_events)
    {
        xQueueReceive(s_events, &event, 0);
    }

    return event;
}
