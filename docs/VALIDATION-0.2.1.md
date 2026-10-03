# 构建验证：Codex Micro 与统一蓝牙设置

日期：2026-10-02。固件版本：0.2.1。

## 已完成

- 运行 `./scripts/build.ps1`，ESP-IDF 编译、链接、分区容量检查、合并固件和复制交付文件均成功，脚本退出码为 0。
- 构建环境为官方 `espressif/idf:v5.5.3`，目标 ESP32-S3；LVGL 9.5.0 和 Brookesia 0.6.0-beta2。
- 应用固件为 5,840,560 字节；8 MiB 应用分区剩余 30%，通过 ESP-IDF 的容量检查。
- 合并固件为 5,906,096 字节；其中 bootloader、分区表、应用字节分别与独立文件在 `0x0`、`0x8000`、`0x10000` 处完全一致。
- 确认合并固件中 NVS 区域 `0x9000..0xefff` 为 `0xff`；烧录该文件会重置本项目设置。
- 使用 esptool 的 `image_info --version 2` 检查，芯片 ID 为 9（ESP32-S3）、Flash 为 16 MB/80 MHz，版本 0.2.1、ESP-IDF v5.5.3；checksum 与 validation hash 均有效。
- 重新计算四个交付二进制的 SHA-256，与 `dist/sha256.json` 全部一致。
- PowerShell 构建、烧录和字体生成脚本通过语法检查。
- 中文字体为不压缩格式，带 Montserrat 符号回退；生成配置启用了大字体描述结构与双 SNTP 服务器。
- 配置确认启用了 Bluedroid BLE 4.2、GATTS、SMP、Wi-Fi/BLE 共存和优先从 PSRAM 分配蓝牙内存；控制器和协议栈运行在 core 0。
- Codex Micro renderer 的宿主测试通过：日夜资源长度、像素字节序转换后重绘一致、离线/手势/熄屏提示状态及画布释放重建 50 次。ASan、UBSan 和泄漏检测未报告错误。该测试验证的是软件画布，不是物理开发板上的整套应用。
- 复制的 Windows Python 后端和 USB 日志采集脚本通过语法编译检查；原生 Codex 蓝牙 RPC 通信已在实机观察到，额度伴生程序同步未单独验收。
- 最后一轮 C/C++ 编译日志未发现 warning 或 error。配置阶段的可选 LVGL 字体/图片依赖条件提示仍存在，相关可选后端未启用。

合并固件 SHA-256：

```text
4159F846E51F6D57548D4094E903231E7282C1B233B520E73B3BE9DF4144DCB3
```

## 实机状态

已在 COM5 的原板上完成 USB 烧录。esptool 识别 ESP32-S3 revision v0.2、8 MiB PSRAM 和 16 MiB Flash；bootloader、分区表和应用三个写入区域均通过设备端数据校验。写入采用独立分区方式，没有擦除 NVS。

第一次启动 0.2.0 时，日志出现 BLE 控制器 `ESP_ERR_NO_MEM` 和 SPI 私有 DMA TX 缓冲分配失败，用户照片显示水平条纹与蓝牙启动失败。0.2.1 的修正见 [COMPATIBILITY.md](COMPATIBILITY.md#on-board-memory-correction-021)。重新编译、校验、烧录后，用户反馈界面已恢复。

第二次复位采集了 90 秒启动和运行日志，确认：

- 实际启动版本为 0.2.1、ESP-IDF v5.5.3，8 MiB PSRAM 内存自检通过。
- LCD 驱动、CST816S 触摸输入、Brookesia 桌面启动，未再次出现 SPI DMA/绘制错误。
- 保存的 Wi-Fi 配置保留，自动连接原网络并获得 IP；亮度恢复为 70%。
- BLE 控制器和 Bluedroid 启动，4 个 GATT 服务、29 个属性注册成功，广播名称为 Codex Micro。
- BLE 约在启动后 28.6 秒连接，29.6 秒完成配对；随后收到 `v.oai.rgbcfg`、`v.oai.thstatus`、`device.status`，回复均为 `failed=0`，后续电量通知确认返回 status=0。
- 电量计 BQ27220 和 ES8311/I2S 提示音通道初始化成功；没有实际播放验收。
- 90 秒日志内没有 NO_MEM、panic、看门狗或非预期重启。

BLE 实时扫描使用隔离的 `.cache/ble-tools` 环境和 Bleak 3.0.2，扫描 12 秒。此时设备已连接，扫描结果为空，因此未将其标记为独立广播发现通过；连接及 RPC 通信由串口日志直接证明。仅枚举 Windows 已缓存设备的结果也未当作新广播证据。

内部 RAM 的测量值：BLE 初始化前空闲 70,499 字节、最大连续块 47,104 字节；初始化后空闲 19,647 字节；62.6 秒时空闲 2,767 字节、最大 DMA 块 1,856 字节，PSRAM 空闲 2,569,640 字节。当前 SPI 刷新缓冲已预分配，启动错误消失，但内部 RAM 余量仍较小，后续多应用/HTTPS/反复连接压力验收需重点检查这一指标。

本次验证范围是 USB 写入、启动、用户确认的画面恢复、Wi-Fi 自动连接和 BLE 配对/RPC。触摸边缘、各应用反复退出、错误密码恢复、路由器断开重连、额度伴生程序同步、实际音频播放及长时间运行尚未完成逐项验收。日志内仍有驱动的 I2C 上拉通用提示及桌面为第五个应用创建新页的提示，未出现相关运行失败。

本地原始记录：`logs/flash-0.2.1-20261002.log`、`logs/boot-0.2.1-20261002.log`、`logs/build-0.2.1.log`。

实机验收项目见 [HARDWARE-CHECKLIST.md](HARDWARE-CHECKLIST.md)。
