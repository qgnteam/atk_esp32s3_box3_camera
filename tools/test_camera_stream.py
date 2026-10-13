"""Test production pause/resume definitions with a modeled V4L2 device.

The camera ownership globals and ioctl are a host fixture. This isolates the
stream lifecycle contract, including borrowed frames and injected ioctl faults;
it does not claim to execute ESP DMA, CameraInit or sensor initialization.
All generated C and binaries stay inside ignored outputs.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "outputs/camera-stream-tests"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "main/camera/camera_capture.c").read_text()
# Compile the actual production definitions verbatim, including getter methods.
functions = source[source.index("esp_err_t CameraPause(void)"):]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
/* Only platform declarations and private-state construction are replaced. */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define CAMERA_BUFFER_COUNT 3
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP 1
#define VIDIOC_STREAMOFF 10
#define VIDIOC_STREAMON 11
#define VIDIOC_QBUF 12
/* Logging consumes tag but does not perform device or console IO. */
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_FALSE(test, error, tag, ...) do { (void)(tag); if (!(test)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(test, tag, ...) do { (void)(tag); int result = (test); if (result != ESP_OK) return result; } while (0)
struct v4l2_buffer { int type; int memory; unsigned index; };
static const char *kTag = "test";
static int s_fd = 7;
static bool s_streaming = true;
static uint8_t *s_buffers[3] = {NULL};
static bool s_borrowed[3] = {false};
static uint32_t s_capture_count = 42;
/* Driver model resets all queues on STREAMOFF just like esp_video 2.5.0. */
static bool s_queued[3] = {true, true, true};
static bool s_driver_streaming = true;
static int s_fault_request = 0;
static unsigned s_fault_queue_index = 0;
static unsigned s_calls[13] = {0};
/* An injected request fails before modifying the driver's state. */
static int ioctl(int fd, int request, void *argument)
{
    assert(fd == s_fd);
    ++s_calls[request];
    if (request == s_fault_request) {
        if ((request != VIDIOC_QBUF) || (((struct v4l2_buffer *)argument)->index == s_fault_queue_index)) {
            s_fault_request = 0;
            errno = EIO;
            return -1;
        }
    }
    switch (request) {
    case VIDIOC_STREAMOFF:
        assert(s_driver_streaming);
        s_driver_streaming = false;
        memset(s_queued, 0, sizeof(s_queued));
        return 0;
    case VIDIOC_QBUF: {
        struct v4l2_buffer *buffer = argument;
        assert(buffer->index < 3);
        /* Duplicate queueing is an error, not silently accepted by the model. */
        assert(!s_queued[buffer->index]);
        s_queued[buffer->index] = true;
        return 0;
    }
    case VIDIOC_STREAMON:
        assert(!s_driver_streaming);
        /* Resume must queue all three slots before enabling capture. */
        assert(s_queued[0] && s_queued[1] && s_queued[2]);
        s_driver_streaming = true;
        return 0;
    default:
        assert(false);
        return -1;
    }
}
/* Resume resets the dequeue timeout; no actual host blocking operation. */
static esp_err_t CameraSetTimeout(unsigned milliseconds)
{
    assert(milliseconds == 1000);
    return ESP_OK;
}
'''
checks = r'''
/* Fresh paused fixture stands in for cleanup/reinitialize after a failed resume.
 * The application must close/reset the device before retrying partial queues. */
static void ResetPaused(void)
{
    s_streaming = false;
    s_driver_streaming = false;
    memset(s_queued, 0, sizeof(s_queued));
}
int main(void)
{
    uint8_t buffers[3] = {0};
    for (unsigned index = 0; index < 3; ++index) s_buffers[index] = &buffers[index];
    /* Borrowed frames must prevent STREAMOFF and preserve capture ownership. */
    s_borrowed[0] = true;
    assert(CameraPause() == ESP_ERR_INVALID_STATE);
    assert(s_calls[VIDIOC_STREAMOFF] == 0 && CameraIsStreaming());
    s_borrowed[0] = false;
    for (unsigned cycle = 0; cycle < 2000; ++cycle) {
        /* Pause really stops the modeled device, and repeated pause is inert. */
        assert(CameraPause() == ESP_OK && !CameraIsStreaming());
        unsigned off_count = s_calls[VIDIOC_STREAMOFF];
        assert(CameraPause() == ESP_OK && s_calls[VIDIOC_STREAMOFF] == off_count);
        unsigned queues = s_calls[VIDIOC_QBUF];
        /* Resume must enqueue exactly three slots; repeated resume is inert. */
        assert(CameraResume() == ESP_OK && CameraIsStreaming());
        assert(s_calls[VIDIOC_QBUF] == queues + 3);
        assert(CameraResume() == ESP_OK && s_calls[VIDIOC_QBUF] == queues + 3);
        assert(CameraGetCount() == 42);
    }
    /* STREAMOFF failure cannot mark an active device as safely paused. */
    s_fault_request = VIDIOC_STREAMOFF;
    assert(CameraPause() == ESP_FAIL && CameraIsStreaming());
    assert(s_driver_streaming);
    assert(CameraPause() == ESP_OK);
    /* Partial QBUF failure must not publish a successful streaming state. */
    s_fault_request = VIDIOC_QBUF;
    s_fault_queue_index = 1;
    assert(CameraResume() == ESP_FAIL && !CameraIsStreaming());
    assert(s_queued[0] && !s_queued[1] && !s_queued[2] && !s_driver_streaming);
    ResetPaused();
    /* STREAMON failure similarly leaves acquisition unavailable to callers. */
    s_fault_request = VIDIOC_STREAMON;
    assert(CameraResume() == ESP_FAIL && !CameraIsStreaming());
    assert(!s_driver_streaming);
    ResetPaused();
    /* Missing mapping or closed fd must fail before touching the driver. */
    s_buffers[0] = NULL;
    unsigned queues = s_calls[VIDIOC_QBUF];
    assert(CameraResume() == ESP_ERR_INVALID_STATE && s_calls[VIDIOC_QBUF] == queues);
    s_fd = -1;
    assert(CameraPause() == ESP_ERR_INVALID_STATE);
    assert(CameraResume() == ESP_ERR_INVALID_STATE);
    puts("PASS: production stream lifecycle; borrowed-frame guard; 2000 cycles; pause/queue/start faults");
    return 0;
}
'''
(OUT / "test.c").write_text(fixture + functions + checks)
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                str(OUT / "test.c"), "-o", str(OUT / "test")], check=True)
subprocess.run([str(OUT / "test")], check=True)
