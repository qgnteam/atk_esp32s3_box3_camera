# 当前版本测试报告

日期：2026-10-13（Asia/Shanghai）；版本：`camera-storage v2.8-resume-black`。仅记录当前版本。

## 编译及烧录

ESP-IDF v6.1 / ESP32-S3构建及USB `/dev/ttyACM0` 烧录通过，Flash校验通过；日志outputs/resume-black-build.log、resume-black-flash.log。当前固件SHA-256：`623c054b641ade2089f4563ec44bccf5cf01d22fb5e484407176f8d960d8975b`。

## 主机检查

| 测试 | 结果 |
|---|---|
| test_preview_warmup.py | 通过；生产WarmupPreview定义配帧模型，恰好丢弃/归还三帧，无效帧也归还，取帧/归还失败不继续；像素使用不可读指针确保没有读取 |
| test_camera_stream.py | 通过；暂停借用帧保护、2000次队列循环及停流/排队/启流错误模型 |
| test_camera_ui.py | 通过；78125格式确认序列、三项一级菜单、TF层级及返回 |
| test_key_filter.py | 通过；三键映射、去抖、两秒边界及长按取消/释放抑制 |
| Python语法及git diff --check | 通过 |

主机模型检查不代表读取了真实LCD或模拟了DMA，硬件正常路径由下述实板日志验证。其余媒体格式/完整AV回归未在本版重新执行，不沿用旧版结果作为本版验收。

## 实板黑屏过渡及菜单验证

`verify_lvgl_menu.py --cycles 10` passed=true。摘要及串口证据位于outputs/resume-black-board/summary.json和serial.log。

- 每次退出顺序均为：PREVIEW_RESUME_BLACK shown → CAMERA_RESUMED → 三次PREVIEW_WARMUP discarded=1/2/3 → PREVIEW_RESUME_SHOWN。
- 三帧已实际归还，未进入StorageDraw；采集元数据即使valid=1也按过渡帧丢弃。
- 黑屏DMA完成到正常预览绘制完成的时长：最短217.01ms、最长225.40ms、平均220.62ms。
- 连续10次菜单切换内存稳定，内部堆179807字节、PSRAM7487588字节，每次相同。
- 菜单streaming=0、空闲时累计取帧不变，返回取景streaming=1；功能/TF/容量/确认页及真实帧CRC导出通过。
- 无采集错误、异常恢复、重启或看门狗日志。本次未新增、删除或格式化TF文件。

自动检查证明过渡帧没有送入显示路径。用户现场通过BACK TO PREVIEW退出一级菜单后确认：短暂黑屏后正常取景，没有花屏。本次退出菜单花屏修复已通过现场观察验证。

## 未执行及限制

未实板注入黑屏DMA失败/预热失败，未验证扬声器观感、长时间音视频回归、Windows流程或真实格式化。三帧预热不对任意传感器内容异常作自动图像质量判定。原面板固定横线的诊断记录另见lcd-line-diagnosis.md。

生成文件在Git忽略目录，docs未修改，用户暂存区及原有修改保留。
