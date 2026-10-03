# Codex Micro 应用集成

应用入口是桌面中的 **Codex Micro** 图标；图标使用与其他应用一致的终端图案。第一个桌面页面放置原有四个应用，左右滑动可到 Codex Micro 所在页面。

## 原项目与适配

导入自用户指定的 `codex-micro-1.85b` 本地项目，Git HEAD 为 `76018f792796322d25b62fd86db1a9a84d8327b7`。导入时的文件哈希见 `codex-micro-source.json`。该项目源自 [digitsisyph/codex-micro-stopwatch](https://github.com/digitsisyph/codex-micro-stopwatch)，导入文件保留原作者信息与 MIT SPDX 许可。

保留像素日夜背景、六个智能体按钮、Send、四向手势、额度显示及缓存、BLE HID/RPC 和额度 GATT 协议。BOOT 短按发送 Voice Chat，按住 0.7 秒发送语音输入意图，松开停止；按键仅在 Codex Micro 应用前台时生效。进入时已按住 BOOT 必须先松开再按下。离开界面释放已按下的控制，不产生额外短按；释放入队失败时走现有可靠释放通道。

这些操作由电脑端执行，开发板不在本地录制或传输音频。

电量读取使用原项目 BQ27220 逻辑，读取失败时显示未知值。完成提示音继续使用原项目 ES8311/I²S 实现，独立任务播放以避免阻塞 UI 和 BLE 消息处理。

移植没有导入原项目的 `app_main`、屏幕驱动、触摸驱动、Wi-Fi 网页配网和独立 SNTP 初始化。图形画布通过 LVGL 图像显示，从原始面板字节序转换为 LVGL 的本机 RGB565 字节序；屏幕、触摸和 I²C 总线由现有 Waveshare BSP 统一管理。

蓝牙的地址恢复使用 `esp_iface_mac_addr_set(..., ESP_MAC_BT)`，仅设置 Bluetooth 接口地址，避免改动系统 Wi-Fi 地址。协议运行在 ESP-IDF 自带的 Bluedroid 中，未增加外部蓝牙框架。使用的 GATTS/GAP 和 MAC API 在 ESP-IDF 5.5.3 的 `bt` 与 `esp_hw_support` 头文件中存在；原项目 manifest 的 IDF >=5.4 约束也包含 5.5.3。

官方接口依据：[ESP-IDF 5.5.3 GAP API](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/bluetooth/esp_gap_ble.html)、[MAC 地址 API](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/system/misc_system_api.html)。

## 统一设置

- Wi-Fi 和校时继续由 `SystemService` 提供，应用内不再独立配网。
- 设置中的蓝牙开关保存在 NVS；关闭时停止广播并断开 BLE 连接，保留配对信息。蓝牙栈保留初始化状态以便快速重新开启。
- 显示设备名、蓝牙地址、广播与连接状态。电脑中添加设备时选择 **Codex Micro**。
- “重新配对”需要点击两次确认，会断开连接、清除开发板旧配对，再开启广播。电脑端也需要移除旧设备记录并重新配对。
- 蓝牙以 BLE 外设身份提供原项目的 HID 和额度服务；这不是用于连接蓝牙耳机或扫描任意蓝牙设备的界面。

## 应用生命周期

点击顶部“桌面”按钮，或从底边上滑返回。底部 30 像素保留桌面手势，应用中部的四向手势继续发送控制。

退出或暂停时释放 Mic、Joystick 和未结束的按键，停止界面定时器，恢复统一设置的亮度；关闭应用后释放约 253 KiB 画布。BLE 与额度接收服务持续运行，切换应用不会重置电脑连接。

长按 Send 6 秒熄屏；触摸或 BOOT 唤醒。电池供电时空闲 2 分钟降低亮度、5 分钟熄屏；外接电源时分别为 10 和 30 分钟。此逻辑仅在 Codex Micro 应用前台生效，不进入深度睡眠。

Windows 额度伴生程序入口见 `scripts/companion/start-companion.cmd`。程序未自动启动，也未修改电脑配对、自启动任务或系统蓝牙设置。
