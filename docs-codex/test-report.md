# 当前版本测试报告

日期：2026-10-13；版本：`camera-storage v2.4-photo`（高亮最低 0.5 秒调整）。

## 本版构建与烧录

ESP-IDF v6.1、esp32s3：`idf.py build` 成功，`idf.py -p /dev/ttyACM0 flash` 成功，写入哈希验证通过。GC0308 的 RGB565_BE QVGA/VGA 配置均启用。本轮未重新运行与高亮计时无关的主机及完整音视频测试。

## 本版实板测试

`python tools/verify_photo_capture.py --port /dev/ttyACM0 --output outputs/photo-feedback-500ms`：通过。使用串口模拟 K2，沿用实体键的 UI 状态机，实际 FAT32 卡未格式化或删除既有文件。

- `/sdcard/DCIM/IMG00007.BMP`：640×480 BMP，921654 字节，完整读回 CRC `c2790481`；高亮处理耗时 3.723 秒，成功提示保持 3.026 秒。
- `/sdcard/DCIM/IMG00008.BMP`：640×480 BMP，921654 字节，完整读回 CRC `24e47fd3`；高亮处理耗时 3.765 秒，成功提示保持 3.026 秒。
- `/sdcard/DCIM/IMG00009.BMP`：640×480 BMP，921654 字节，完整读回 CRC `37a7062e`；高亮处理耗时 3.725 秒，成功提示保持 3.026 秒。

三次均按顺序完成高亮帧绘制、VGA 采集、TF 保存、结束反馈及 QVGA 恢复。高亮处理耗时包含相机切换和 TF 写入，当前卡超过 0.5 秒，未追加最低时长等待；普通画面在摄像头恢复后刷新。PHOTO SAVED 仍从首次成功绘制起保持 3 秒。

证据：`outputs/photo-feedback-500ms/serial.log` 和 `summary.json`。串口运行无崩溃或应用相机恢复。组件重复初始化时仍输出 GPIO ISR 服务已安装日志，采集及保存正常。

## 验证边界

当前硬件捕获及写入耗时超过 0.5 秒，因此本轮未实测快速保存时补足最低时长的分支。该分支按实际高亮绘制时间计算剩余微秒，仅在剩余时间大于零时等待。新计时版本的屏幕观感及物理按键操作未单独人工确认。当前实卡为 FAT32，不计为 exFAT 实卡验收；未进行卡满、掉电和长期运行验收。
