# Waveshare 1.85B 应用桌面

为原版 ESP32-S3-Touch-LCD-1.85B 制作的应用固件：开机画面 → Brookesia 应用桌面 → 设置、时钟、网络测试、设备信息、Codex Micro。

构建使用 ESP-IDF 5.5.3，输出见 `dist/`；构建版本与 USB 实机验证结果见 [docs/VALIDATION.md](docs/VALIDATION.md)。触摸操作、配网与电脑控制仍需逐项验收。

## 当前功能

- 中文界面、独立应用图标、底部上滑回桌面、应用内返回按钮。
- 设置中扫描 2.4 GHz Wi-Fi、触屏输入密码、手动输入 SSID、忘记网络。
- 成功获得 IP 后保存网络凭据，重启自动连接，断线后按 2/4/8/16/30 秒间隔重试；单次连接最多等待 20 秒。
- 所有应用共用一份 Wi-Fi 状态和网络连接；切换应用不重置 Wi-Fi。
- 亮度实时调整，松手后保存，开机恢复。
- 联网后通过 SNTP 同步北京时间；未同步时显示 `--:--:--`。
- 网络应用分别报告 DNS、校时、HTTPS 证书验证、响应码与耗时；首次联网且校时成功后自动检查一次。
- 设备信息显示电池估算电量、原始 SOC、电压、电流、平均电流/功率、温度、剩余/满充/设计容量、健康度、循环次数、预计充放时间、建议充电参数和状态寄存器；另有版本、MAC 与内部 RAM/PSRAM。
- Codex Micro 像素仪表盘：日夜主题、智能体选择、Send、四向控制、BOOT 语音操作、额度缓存和同步。
- 统一设置中的蓝牙 BLE 开关、设备地址、连接状态和重新配对；蓝牙与额度服务在切换应用后继续运行。
- Codex Micro 内的板载电量读取与完成提示音；电量无法读取时显示未知。
- 小智助手：激活码、WebSocket/Opus 语音会话、识别与回复文本、BOOT 按键说话，以及设备内的八个 MCP 工具。
- 音乐播放器：内置耀狐网易点歌 HMAC 桥接与 FFmpeg PCM 转码；小智点歌后自动切换音乐界面，暂停后返回小智。
- 共享设置新增助手服务地址、令牌、音乐服务地址和音量；所有应用继续共用原有 Wi-Fi，不重复配网。
- 音频优先级为 Codex 完成提示音 > 小智语音 > 音乐；音乐播放时保留小智后台收音，以接收暂停指令。

当前未加入天气、相册、固件 OTA 升级和 RTC 断电保时。小智的激活服务接口仅用于获取激活信息和连接地址。桌面状态栏的电池图标暂时隐藏。

小智激活、音乐服务格式、MCP 工具及测试方法见 [docs/AI-ASSISTANT.md](docs/AI-ASSISTANT.md)。语音与扬声器效果仍需实机验收。

## 修改后的渲染检查

每次修改界面，先生成并检查桌面和图标，再编译和 USB 烧录：

```powershell
./scripts/preview-ui.ps1
```

需安装 Pillow（`python -m pip install Pillow`）。输出在 `.cache/ui-preview/`：
`desktop-round.png` 为两页桌面的圆屏预览，`desktop-1.png`、`desktop-2.png` 为完整帧缓冲，`icons.png` 为图标检查图，`device-info.png` 为设备信息预览（电池输入为检查夹具）。
该脚本编译的是宿主 LVGL 9.5 渲染器，直接使用固件图片和中文字体、`ui_layout.hpp` 的布局尺寸以及 Brookesia 的排版算法；它不模拟手机应用管理器、触摸或硬件屏幕。
原生检查会校验全部图标与文字在圆形可视范围内，并检查电池文本中的单位与未知值。
必须打开预览确认圆角透明、文字完整、图标对齐和分页点位置后，再执行固件构建及烧录。

## 编译

需要 Docker Desktop 启动并使用 Linux containers。构建采用官方 `espressif/idf:v5.5.3` 镜像，首次需要下载镜像和组件。已有的 Windows ESP-IDF 安装不会被改动。

在项目根目录运行：

```powershell
./scripts/build.ps1
```

输出在 `dist/`：

- `waveshare-launcher-usb.bin`：合并固件，Flash Download Tool 烧录地址 `0x0`。该文件包含 NVS 所在地址的空白填充，重新烧录会重置配网、亮度与蓝牙配对。电脑端保留旧配对时，请移除旧设备再添加。
- `bootloader.bin`、`partition-table.bin`、`waveshare_launcher.bin`：分别烧录至 `0x0`、`0x8000`、`0x10000`。脚本采用这个方式，后续升级保留本项目设置。
- `sha256.json`：固件校验值。

遇到缓存问题可以运行 `./scripts/build.ps1 -Clean`。`firmware/sdkconfig.defaults` 和 `dependencies.lock` 用于复现配置及组件版本。

## 健身应用（0.3.0）

桌面第二页新增「健身」：完整周计划、14 个离线循环动作示意、点击记组、组间休息和最近 30 次训练。设置可调整每动作目标和周计划模板，退出桌面保留训练，重启后恢复进度。详细操作、资源生成、验证及实机验收项见 [docs/FITNESS.md](docs/FITNESS.md)。

## USB 烧录

先确定设备 COM 端口。以下 `COM5` 是命令示例，需换成开发板实际端口。

```powershell
python -m pip install esptool
./scripts/flash.ps1 -Port COM5
```

下载失败时按住 BOOT，插入 USB 或复位，再松开 BOOT 后重试。完成后复位设备。

此固件会替换设备原有应用与分区表。首次从其他固件迁移时，如果日志提示 NVS 初始化失败，可通过烧录合并固件重置本项目配置；程序本身不会自动擦除 NVS。

日志使用原生 USB Serial/JTAG。通过串口工具以 115200 波特率查看日志即可。首版不额外占用 UART 扩展接口。

已有 ESP-IDF Python 环境时，可用 `-Python` 指定其 `python.exe`，无需修改系统 Python。例如本机使用：

```powershell
./scripts/flash.ps1 -Port COM5 -Python 'D:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe'
& 'D:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe' scripts/monitor.py --port COM5 --reset --seconds 30 --output logs/boot.log
```

`monitor.py --reset` 会复位原板并捕获指定时长的启动日志；省略 `--reset` 可观察正在运行的固件。

## 使用

1. 开机后点击“设置”。
2. 点击“扫描 Wi-Fi”，选择网络，输入密码，点击“连接”。
3. 出现“已连接，所有应用共享网络”后返回桌面。
4. 打开“时钟”等待校时；打开“网络测试”检查 HTTPS 请求。
5. 应用内点击“返回”，或从屏幕底部上滑回桌面。应用可能保留在最近应用中，可在最近应用界面上滑关闭。
6. 设置中开启蓝牙，在 Windows 中添加 **Codex Micro**；双击 `scripts/companion/start-companion.cmd` 自动启动本项目附带的网桥并持续同步额度，不再依赖原项目目录，配置及检查方式见 `scripts/companion/README.md`。
7. 桌面左右滑动可找到 Codex Micro 图标。仪表盘顶部“桌面”按钮可返回。

“忘记当前网络”需要连续点击两次确认。手机热点请设为 2.4 GHz；首版针对开放网络和常见个人密码 Wi-Fi，不支持需要网页登录的网络和企业证书配网。

蓝牙开关关闭时停止广播并断开连接，配对信息保留。“重新配对”需要点击两次，会清除开发板旧配对；电脑端也需移除旧设备后重新添加。蓝牙提供 Codex Micro 的 BLE 外设服务。完整集成说明见 [docs/CODEX-MICRO-INTEGRATION.md](docs/CODEX-MICRO-INTEGRATION.md)。

## 代码结构与新增应用

```text
firmware/
  main/main.cpp             开机画面、桌面、状态栏和应用注册
  main/launcher_apps.*      首版四个应用及配网界面
  main/system_service.*     Wi-Fi、NVS、SNTP、后台 HTTPS 工作
  main/bluetooth_service.*  BLE、额度、电量与提示音的后台服务
  main/codex_micro_app.*    Codex Micro 的桌面应用与生命周期
  main/ui_font.c            中文字体，包含常用 CJK 字符
  main/app_icons.c          原创图标
  main/ui_app_shell.hpp     应用安全边距、标题、返回与滚动提示
  main/battery_info.hpp     电量计读数的共享格式化
  main/dark/                桌面样式
  components/               Brookesia、板级驱动及导入的 codex_micro
  partitions.csv            USB 单应用分区布局
scripts/                    构建、烧录和资源生成脚本
docs/                       兼容依据、字体许可与实机检查清单
```

新增应用继承 `esp_brookesia::systems::phone::App`，通过 `phone->installApp()` 注册。UI 在 `run()` 中创建；用 `notifyCoreClosed()` 返回；`run()` 中的页面和 LVGL 定时器由 Brookesia 统一回收。其他任务不得直接操作 LVGL 对象。

网络状态从 `SystemService::instance().snapshot()` 获取；共用 ESP-IDF 网络栈进行请求。新增应用不得再次初始化 Wi-Fi、默认 netif、事件循环或另行保存 Wi-Fi 密码。较长请求放到后台任务，退出时取消或解除 UI 引用。

蓝牙状态和操作通过 `BluetoothService` 获取与入队；新增应用不要再次初始化蓝牙控制器或注册全局 GAP/GATTS 回调。

图标由 `python scripts/generate_icons.py` 生成。字体使用 Noto Sans SC，遵循 `docs/FONT-LICENSE.txt` 的 SIL OFL；修改字体资源可运行 `./scripts/generate_font.ps1`，需要 Node.js。正常构建不需要重新生成资源。

固件打包后可运行 `python scripts/verify_artifacts.py` 检查版本、哈希、分区地址与容量。Codex Micro 图形和画布生命周期的宿主测试可在同一 Docker 镜像中运行 `bash scripts/test-render.sh`。

官方源码基线及依赖依据见 [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)。构建、渲染和 USB 启动检查证据见 [docs/VALIDATION.md](docs/VALIDATION.md)；完整交互与压力验收的剩余项目见 [docs/HARDWARE-CHECKLIST.md](docs/HARDWARE-CHECKLIST.md)。
