# 当前版本测试报告

日期：2026-10-12。版本：`camera-storage v1.2-exfat`。测试端口：COM4。范围：GC0308 QVGA取景、BMP拍照、无声BGR24 AVI录像、TF SPI存储及exFAT编译支持。

## 结论与实际卡格式

最终固件编译、烧录及Flash数据哈希校验通过。当前插入TF卡实际挂载类型为 **FAT**，日志 `TF mounted at /sdcard/DCIM fs=FAT`。已经在此卡完成4张照片、短录像、60秒自动停止录像和每个文件的完整读回。**exFAT代码已启用，但exFAT卡的实物挂载/写入仍未验证**；需用户换入exFAT卡，或明确允许清空当前卡并格式化。程序未格式化当前卡，也未覆盖原有文件。

## 构建和烧录证据

ESP-IDF v6.1，Xtensa esp-15.2.0_20251204；esp_video 2.5.0、esp_cam_sensor 2.6.0。CMake生成项目内FatFs覆盖组件并由build/project_description.json确认实际采用；全局SDK未修改。覆盖FF_FS_EXFAT=1，开启LFN，并修复关闭卷标时的FF_USE_LABEL宏；C静态断言通过。

最终镜像：`build/box3_camera_preview.bin`，384736字节（0x5dee0），1MiB应用分区剩余约63%。SHA256：`cb6a22088adb8cb22f5ab866938e3ba408cff7049775eb05e3b4d31378e4f044`。日志：outputs/build-exfat.log、outputs/flash-exfat.log。最终构建无编译warning/error，烧录日志 `Hash of data verified`。

GPIO：TF SCLK15/MOSI16/MISO17/CS18，LCD共用SPI2、CS47。最终固件识别AW9523B ID0x23、GC0308 PID0x9b，RGBR 320×240 stride640，8MiB PSRAM自检通过；应用首帧约993ms。

## 板上写入与读回

串口p/r/v调用与物理按键相同的存储逻辑，v重新打开最终文件，从头到尾读取并比较BMP/RIFF声明长度，输出CRC。CRC用于记录完整读回内容，不代表与原采集帧逐字节比对，也不能替代肉眼图片或播放器验证。

| 文件 | 内容/长度 | 完整读回CRC | 结果 |
|---|---|---|---|
| DCIM/IMG00001.BMP | 230454字节 | 8fdea588 | valid=1 |
| DCIM/IMG00002.BMP | 230454字节 | 6c79ac05 | valid=1 |
| DCIM/VID00001.AVI | 10帧，2304472字节 | 2ed4a04e | valid=1 |
| DCIM/IMG00003.BMP | 230454字节 | b9e77325 | valid=1 |
| DCIM/IMG00004.BMP | 230454字节 | a2a9cf52 | valid=1 |
| DCIM/VID00002.AVI | 93帧，21429664字节 | 0109f4c8 | valid=1 |

短录像平均周期664769µs，约1.50FPS；60秒录像平均周期648801µs，约1.54FPS。录像期间取景约8FPS。最大5FPS是采样上限，不是此卡的实测性能承诺。长录像日志RECORD_START在约2504ms，RECORD_STOP约62664ms，约60.16秒自动停止。读回21.4MB文件约23.7秒，这期间取景暂停处理。

两次测试间USB软件复位，第二轮使用IMG00003/04和VID00002，未覆盖第一轮文件。串口测试未操作物理BOOT按键，屏幕标记、照片方向/色彩及桌面播放器兼容尚未人工确认。

命令与日志：

```powershell
. ./tools/idf-env.ps1
python tools/verify_storage.py --port COM4 --record-seconds 6 --allow-fat --log outputs/storage-current-card.log
python tools/verify_storage.py --port COM4 --record-seconds 61 --allow-fat --log outputs/storage-auto-stop.log
python tools/serial_verify.py --port COM4 --seconds 30 --log outputs/preview-exfat-firmware.log
```

两次存储测试JSON `passed=true`、`filesystem=FAT`。默认去掉--allow-fat会要求exFAT，当前卡不能通过该项。

完成录像和读回后的30秒续采：29个STATS样本，FPS最小/最大/平均均为24.00，display_ms平均32.65，未出现错误、恢复或崩溃。累计帧5824→6524；29种CRC；内部RAM空闲276659字节、PSRAM空闲7905508字节均保持稳定。该项仅覆盖30秒，不替代长时间稳定性测试。

## 验证中修复及运行提示

最初串口输入未响应；改为直接驱动读取时，实机发现默认配置没有安装USB读驱动，触发一次LoadProhibited。最终版本增加is_driver_installed检查、显式驱动安装、VFS切换和读取保护；修复后重新编译烧录，两轮真实存储测试通过。上述失败版本未作为最终交付。最终运行仍出现CS18/CS47由初始GPIO输出移交设备时的冲突提示，卡初始化CMD52/CMD5不支持提示属于普通SD卡探测；没有影响本次成功读写。

主机格式测试此前运行实际C转换代码，覆盖全部65536种RGB565颜色，验证BMP头、12组AVI头及RIFF/idx1偏移；本次未修改格式代码。Git排除build/outputs/sdkconfig/managed_components，覆盖组件、测试依赖和日志不进入Git。

## 尚未完成

- exFAT实物卡挂载、拍照和录像读回。
- 物理BOOT短按/长按、屏幕红色标记及图片方向颜色确认。
- 桌面播放器播放AVI，验证显示内容与时间。
- 无卡/卡满/突然拔卡故障注入、掉电恢复及长时间连续运行。
- 断电冷启动；当前复位测试仅USB软件复位。

自动写卡及文件读回不能替代以上验收。本版不会自动转换文件系统或格式化卡。
