"""Execute production WarmupPreview with modeled frame ownership and failures.

Does not simulate actual LCD appearance or DMA. Poisoned pixel pointers ensure
warmup never reads image data; every dequeued frame must be returned, including
invalid frames. Files remain inside ignored outputs.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "outputs/preview-warmup-tests"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "main/application.c").read_text()
start = source.index("static esp_err_t WarmupPreview(void)")
end = source.index("\n/**", start)
production = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Only platform callbacks are replaced; the function body is production code. */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define PREVIEW_RESUME_DROP_FRAMES 3
#define ESP_LOGI(tag, ...) do { (void)(tag); CheckLog(__VA_ARGS__); } while (0)
static const char *kTag = "test";
typedef struct { const uint8_t *data; unsigned index; bool valid; } CameraFrame;
static unsigned s_gets = 0;
static unsigned s_returns = 0;
static unsigned s_get_failure = 0;
static unsigned s_return_failure = 0;
static bool s_borrowed = false;
/* Three frames are consecutive arrivals; one intentionally has invalid flags.
 * The pointer faults if dereferenced: discarded pixels must never be inspected. */
/* Progress may be logged only after the frame has been successfully returned. */
static void CheckLog(const char *format, unsigned discarded, unsigned index, int valid)
{
    (void)format;
    assert(!s_borrowed);
    assert(discarded == s_returns && index == s_gets - 1);
    assert(valid == (s_gets != 2));
}
static esp_err_t CameraGet(CameraFrame *frame)
{
    assert(!s_borrowed);
    ++s_gets;
    if (s_gets == s_get_failure) return ESP_FAIL;
    frame->data = (const uint8_t *)(uintptr_t)1;
    frame->index = s_gets - 1;
    frame->valid = (s_gets != 2);
    s_borrowed = true;
    return ESP_OK;
}
/* Successful return revokes ownership and clears the frame like CameraReturn.
 * Failed return keeps ownership, requiring the caller to stop/recover capture. */
static esp_err_t CameraReturn(CameraFrame *frame)
{
    assert(s_borrowed && frame->data != NULL);
    ++s_returns;
    if (s_returns == s_return_failure) return ESP_FAIL;
    s_borrowed = false;
    memset(frame, 0, sizeof(*frame));
    return ESP_OK;
}
'''
checks = r'''
/* Reset is a fixture boundary, not a claim that production can ignore a lease.
 * Real recovery must stop DMA before it releases mapped frame resources. */
static void Reset(void)
{
    s_gets = 0;
    s_returns = 0;
    s_get_failure = 0;
    s_return_failure = 0;
    s_borrowed = false;
}
int main(void)
{
    /* Invalid transition frames still return; no warmup pixel reaches display. */
    Reset();
    assert(WarmupPreview() == ESP_OK);
    assert(s_gets == 3 && s_returns == 3 && !s_borrowed);
    /* A dequeue failure immediately ends warmup without leaking a prior frame. */
    Reset();
    s_get_failure = 2;
    assert(WarmupPreview() == ESP_FAIL);
    assert(s_gets == 2 && s_returns == 1 && !s_borrowed);
    /* A return failure cannot announce ready or continue borrowing more frames. */
    Reset();
    s_return_failure = 2;
    assert(WarmupPreview() == ESP_FAIL);
    assert(s_gets == 2 && s_returns == 2 && s_borrowed);
    puts("PASS: production preview warmup; three returns; invalid frames; dequeue/return faults; no pixel reads");
    return 0;
}
'''
(OUT / "test.c").write_text(fixture + production + checks)
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                str(OUT / "test.c"), "-o", str(OUT / "test")], check=True)
subprocess.run([str(OUT / "test")], check=True)
