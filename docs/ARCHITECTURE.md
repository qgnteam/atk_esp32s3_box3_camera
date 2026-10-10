## 架构
- main/application.*：主事件循环、协议生命周期和高层行为。
- main/device_state_machine.*：运行时允许的状态转换。
- main/boards/common/：开发板接口，以及可复用的硬件和网络辅助模块。
- main/boards/**/：各开发板专用的引脚定义、初始化逻辑和构建变体。
- main/audio/：音频编解码器、音频任务、引擎、唤醒词和队列。
- main/protocols/：与传输方式无关的 API，以及 WebSocket 和 MQTT/UDP 实现。
- main/display/ 和 main/led/：可复用的用户界面实现。
