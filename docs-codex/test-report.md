# 当前版本测试报告

日期：2026-10-13。版本：`camera-storage v2.3-keys`，菜单底部单行修订。本报告仅记录本次修改的验证。

## 修改与结果

媒体列表底部同一行显示 `K0: DOWN  K1: UP  K2: OPEN  HOLD K2: CANCEL`，依次对应K0、K1、K2、HOLD K2。实际按键功能沿用当前实现。

- ESP-IDF v6.1编译成功，固件437472字节。证据：outputs/build-footer-single.log。
- 通过/dev/ttyACM0烧录成功，写入哈希验证通过。证据：outputs/flash-footer-single.log。
- 开发板启动并挂载FAT32卡，进入含39个已有文件的菜单，状态screen=3。
- 导出实际提交给LCD的153600字节RGB565菜单缓冲，长度及CRC核验通过，CRC为de4bb722。
- 解码截图确认提示顺序为K0、K1、K2、HOLD K2，四项文字在同一行完整显示且没有显示长按时长。截图：outputs/board-footer-single/menu.png；串口日志及结果：outputs/board-footer-single/serial.log、summary.json。
- git diff --check通过。

固件SHA256：fe5283ed3840043874b78e1e6b752f53c9baed4c360dc4880b257f6f955e3268。

本次检查只读取已有媒体并导出界面，未创建、删除或格式化TF卡文件。截图检查验证LCD提交内容；实体面板观感未在主机侧直接观察。本次仅调整提示顺序，未重复录音、格式化和物理长按测试。
