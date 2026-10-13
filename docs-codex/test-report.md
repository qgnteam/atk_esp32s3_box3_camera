# 当前版本测试报告

日期：2026-10-13（Asia/Shanghai）；版本：`camera-storage v2.9-menu-keys`，最新菜单方向为 K1 下翻、K2 上翻。本报告仅保留最新实现的验证结果。

## 编译

ESP-IDF v6.1 / ESP32-S3 `idf.py build` 通过，日志 `outputs/menu-down-up-build.log`。
固件 `build/box3_camera_preview.bin`；SHA-256：`df94f2f1662d0ce19c583cf154aa375266fbc73d0a78c481e9f891303e07acd3`。

本次已烧录至 `/dev/ttyACM0`，Flash 写入哈希校验通过，日志 `outputs/menu-down-up-flash.log`。未新增、删除或格式化 TF 文件。

## 主机验证

| 检查 | 结果 |
|---|---|
| `tools/test_key_filter.py` | 通过；实际 C 过滤器及实体页面映射，所有菜单页 K0 确认/取消、K1 下、K2 上；功能/TF 菜单每一行的下一项/上一项及首尾循环；媒体相邻文件选择和打开/返回 |
| 格式确认及长按回归 | 通过；K1 选择 YES、K2 选择 CANCEL，K0 分别确认两页才产生格式化；长按取消在 2 秒触发且释放不确认，取景布局保持原行为 |
| `tools/test_camera_ui.py` | 通过；78125 格式确认序列、功能/TF 层级、默认取消及循环选择 |
| `tools/test_lvgl_menu.py` | 通过；实际 LVGL 页面、焦点、缩略图、输出边界、双确认、销毁/重建与 1000 次渲染内存稳定 |
| `git diff --check` | 通过 |

日志 `outputs/menu-down-up-filter.log`、`outputs/menu-down-up-ui.log`、`outputs/menu-down-up-lvgl.log`；主机渲染图像在 `outputs/lvgl-tests`。产物处于 Git 忽略目录。

## 实板验证

- 首次复位启动失败：AW9523B ID 读取返回 ESP_ERR_INVALID_RESPONSE，未进入取景；证据 `outputs/menu-down-up-boot.log` 与 `.json`。再次复位正常，未修改代码。此次通信失败原因尚未定位，不将首次启动记录为通过。
- 重试启动/取景通过：首帧 1229 ms，15 秒平均 23.82 FPS，12 个统计样本，丢帧 0，无异常日志，内部堆和 PSRAM 稳定；证据 `outputs/menu-down-up-boot-retry.json` 与 `.log`。
- `verify_lvgl_menu.py --cycles 10` passed=true：功能/TF/容量/格式确认页输出 CRC、默认 CANCEL、串口确认阻断与菜单停流检查通过。
- 10 次退出菜单均按黑屏 → 恢复采集 → 丢弃/归还三帧 → 正常显示执行；黑屏 217.094～231.949 ms，内存各轮一致（内部堆 179807 字节、PSRAM 7487588 字节）。
- 实板导出实际 LCD 缓冲已检查，底栏完整显示 `K0:OK K1:DOWN K2:UP HOLD K0:BACK`。截图不代表面板像素读回。
- `verify_media_browser.py` passed=true：已有文件分页、缩略图、BMP 打开返回、AVI 播放启动/取消及资源回收通过，仅读取媒体。

菜单/媒体证据在 `outputs/menu-down-up-board`、`outputs/menu-down-up-media` 的 summary.json、serial.log 及截图。

## 验证范围及限制

实体菜单事件经 CameraUiPhysicalKey 转换；串口 e/l/n/b 仍分别为逻辑确认/上/下/取消（菜单实体对应 K0/K2/K1/K0 长按）。格式确认页继续禁止串口模拟控制。屏幕提示已同步为 `K0:OK K1:DOWN K2:UP HOLD K0:BACK`。

实板自动验证使用串口逻辑事件，实体 K1/K2 方向由真实 C 过滤器/页面映射主机测试覆盖，未人工验证实体按压及面板观感。未执行媒体写入、完整音视频或 Windows 回归。docs 下文件及用户暂存的编辑器配置未修改。

测试结束设备已返回拍照取景，`UI_STATUS screen=0 streaming=1`；证据 `outputs/menu-down-up-final-state.log`。
