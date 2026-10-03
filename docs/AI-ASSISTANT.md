# 小智助手、音乐与本地 MCP

## 集成范围

本项目保留现有 Brookesia 桌面、共享网络服务与 Codex Micro，新增「小智助手」和「音乐播放器」。不是将小智完整固件替换到设备上。

- 小智协议参考 `https://github.com/78/xiaozhi-esp32`，检查的上游提交为 `0d576d3d4c049c6f55eaf879725dc23e516511b4`。
- 实现上游 WebSocket v1、JSON hello/listen/STT/TTS/MCP、Opus 二进制音频，以及激活服务的连接信息获取。
- 组件固定为 `78/esp-opus 1.0.5`、`espressif/esp_websocket_client 1.7.0`，构建使用 ESP-IDF 5.5.3。
- 音乐协议参考 `D:\Desktop\codex\codex-quota-widget\firmware\esp32-ornament\main\music_player.c`，沿用检索与原始 PCM 流播放方式，不直接播放 MP3。
- 当前不包含离线唤醒词、AEC 回声消除、完整小智 OTA 升级、MQTT 音频传输或 WebSocket v2/v3 二进制帧。服务器需接受 v1。

## 首次使用

1. 在桌面「设置」中连接 Wi-Fi。小智和音乐只消费 `SystemService` 的共享网络状态，不创建第二个 Wi-Fi 服务。
2. 在同一设置页向下滚动到「AI 助手与共享音频」。默认激活服务地址为 `https://api.tenclass.net/xiaozhi/ota/`。
3. 保持自建 WebSocket 地址为空，打开「小智助手」并点击「开始对话」。若显示激活码，在小智账户的设备激活页面完成绑定，再点击「开始对话」。
4. 使用自建服务时，填写 WebSocket 地址和可选令牌，保存后重新开始对话。非 TLS 的 `ws://`、`http://` 仅建议用于可信局域网。
5. 填写音乐服务基础地址并保存，例如 `http://电脑IP:端口/music`。服务需可从开发板的网络访问。
6. 可以说「打开 Codex Micro」「打开音乐播放器」「播放某首歌」「暂停音乐」。实际自然语言到 MCP 工具的选择由小智服务端完成。

### 网易点歌 API 桥接

设备只接收 16 kHz、单声道、signed 16-bit little-endian PCM，不能直接播放点歌 API 返回的 MP3。仓库提供 `scripts/music_bridge.py`：使用耀狐 API 的时间戳 HMAC-SHA256 鉴权查歌，再调用 FFmpeg 转码为设备所需 PCM。

1. 将 `scripts/music-bridge.config.example.json` 复制到 `.cache/music-bridge/config.json`，填写本机 WLAN IPv4、随机访问令牌、API key 和 secret key。`.cache/` 已被 Git 忽略；不要把真实凭据写入仓库。
2. 安装 FFmpeg，并运行 `./scripts/start-music-bridge.ps1`。服务只允许绑定具体的私有 IPv4，并只接受携带随机路径令牌的局域网请求。
3. 用 USB 配置设备：`python scripts/assistant-console.py --port COM5 --music-config .cache/music-bridge/config.json --music-command status --seconds 5 --output logs/music-configured.log`。
4. `scripts/companion/start-companion.ps1` 检测到私有配置后会自动保持音乐桥接服务运行。

API key、secret key 和随机令牌只保存在本机忽略目录；设备 NVS 仅保存带随机令牌的局域网桥接地址。若电脑 IP 改变，需要更新私有配置并重新执行第 3 步。

「开始对话」启用服务端自动结束语句模式。也可在小智页面按 BOOT 开始说话，松开结束，或点击「打断并聆听」「说完了」。「停止小智」终止语音连接，不影响音乐和 Codex 提示音。

BOOT 仅在小智助手处于前台且当前屏幕属于该应用时处理，采用 30 毫秒消抖。按住 BOOT 进入或恢复页面时，必须先松开再重新按下。离开页面会结束本次 BOOT 操作启动的录音，暂停后再关闭不会重复发送结束；不会结束由其他按钮或后台操作接管的会话。开始请求入队失败不会记录为正在按住；结束请求通过独立会话标记交给服务任务处理，不受普通操作队列满的影响。蓝牙和小智消息格式保持原样。

设置和稳定的 Client-Id 存于设备的 `voice_apps` NVS 命名空间；Wi-Fi 凭据仍由原有共享服务保存。屏幕中的令牌输入是密码框，状态查询不会返回令牌。NVS 当前没有加密；不要把设备共享给不信任的人。

## 用户指定的音频与页面行为

| 场景 | 行为 |
| --- | --- |
| Codex 任务完成 | 高优先级提示音任务抢占共享输出；不允许语音或音乐覆盖提示音 |
| 小智请求播放音乐 | 异步开始查歌并切换至音乐播放器；小智页面待机，后台语音连接与收音保留 |
| 小智讲话 | 暂时压制音乐输出，语音结束后音乐继续；Codex 提示音仍可抢占 |
| 语音或按钮暂停音乐 | 暂停当前流并保留歌曲、播放位置，返回小智页面；在音乐页可点击继续播放 |
| 停止音乐 | 取消当前流、释放 HTTP 客户端，返回小智页面 |
| 返回桌面或其他应用 | 不自动终止后台语音与音乐；需使用相应停止按钮/工具 |

由 `shared_audio` 唯一创建 BSP 的 ES8311/ES7210 与 I2S 驱动。硬件统一使用 24 kHz、16-bit 单声道；麦克风上传转换为 16 kHz，音乐的 16 kHz PCM 转换到共享输出采样率。提示音抢占及语音压制期间，低优先级样本被丢弃而不是混音，并按样本时长节流，避免音乐流被快速耗尽。

「小智待机」不是关闭麦克风：保持收音是接收「暂停音乐」所必需的。未实现本地 AEC，播放音量较大时可能影响识别。可降低音量，或者点击音乐页的「暂停并返回小智」作为可靠的触屏入口。音量为 0 或原有提示音开关关闭时，尊重静音设置。

## 音乐服务契约

基础地址后追加：

- `GET /resolve?song=<UTF-8编码歌名>&artist=<可选歌手>`，返回 JSON。
- 若 JSON 未提供 `url`，播放地址为 `GET /stream?song=...&artist=...`。

示例响应：

```json
{
  "ok": true,
  "title": "歌曲名称",
  "artist": "歌手",
  "sampleRate": 16000,
  "channels": 1,
  "url": "http://电脑IP:端口/music/stream?song=example"
}
```

`url` 可省略。`sampleRate`/`sample_rate` 和 `channels` 若提供，必须分别为 16000 和 1。流必须是 **16 kHz、单声道、有符号 16-bit little-endian 原始 PCM**，不含 WAV 文件头，Content-Type 为 `audio/pcm`、`audio/x-raw` 或 `application/octet-stream`。固件拒绝常见 HTML/JSON、WAV、MP3 标签、Ogg 和 FLAC 内容，避免将压缩文件当成 PCM 播放噪声。

播放、暂停、检索在独立任务运行，不阻塞 LVGL，也不重置 Wi-Fi。暂停保留 HTTP 流；若服务端在长时间暂停时断开连接，继续播放可能失败，需要重新点歌。

## 本地 MCP 工具

工具目录在 `firmware/main/mcp_tools.json`，设备通过小智 WebSocket 的 `type: mcp` 信封处理 JSON-RPC 请求，并非开放到局域网的未认证 HTTP MCP 服务。

| 工具 | 参数 | 用途 |
| --- | --- | --- |
| `self.codex.open` | 无 | 打开 Codex Micro |
| `self.music.open` | 无 | 打开音乐播放器，不自动播放 |
| `self.apps.open` | `app`: `codex/music/assistant/settings` | 切换应用 |
| `self.music.play` | 必填 `song`，可选 `artist` | 点歌并跳转音乐界面 |
| `self.music.pause` | 无 | 暂停并返回小智 |
| `self.music.stop` | 无 | 停止并返回小智 |
| `self.audio.set_volume` | 整数 `volume`: 0–100 | 调整并保存共享音量 |
| `self.device.get_status` | 无 | 返回共享网络、语音、音乐、音量与提示音优先状态 |

歌名/歌手限制为最多 120 UTF-8 字节。工具采用白名单和严格参数检查，不能执行任意 shell 命令。通知不返回响应，也不执行工具。操作响应表示已受理；网络播放和设置持久化的后续结果显示在相应应用中。页面切换通过队列交给 LVGL 线程，网络回调不直接操作界面。

## 验证

### USB 连接诊断

设备 USB 控制台支持 `xiaozhi status`、`xiaozhi connect`、`xiaozhi stop`、`xiaozhi open` 和 `help`。这些命令只操作本地助手，不提供任意 shell 执行能力。串口不能同时被监视器、烧录工具和控制台占用。

```powershell
& 'D:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe' scripts/assistant-console.py --port COM5 --command status --command connect --seconds 45 --output logs/xiaozhi-connect.log
```

日志按阶段记录 OTA 的 HTTP/TLS 结果、麦克风初始化、WebSocket 事件与关闭码、服务器 hello 和音频采样率，不记录 Wi-Fi 密码或服务令牌。若 OTA 返回激活码，它会显示在助手页面，也可通过 `xiaozhi status` 的 `activation` 字段查看；返回 `none` 时说明当前没有收到激活码，不能凭空生成六位验证码。

2026-10-03 实机排查发现：共享网络状态中的 MAC 使用大写，直接用于小智的 `Device-Id` 会在 HTTP 101 升级后被服务器立即关闭。使用同一设备和令牌进行大小写对照测试，小写标识收到服务器 hello 和 MCP 初始化请求，大写标识收到空关闭帧。助手现在只对小智 OTA 请求头、请求体及 WebSocket 请求头统一使用小写 MAC，保留共享网络、蓝牙与电脑桥接原有标识格式。对应回归测试为 `tests/audio/xiaozhi_protocol_test.cpp`。

连接恢复后进一步暴露了两个内存问题：TLS 音频发送报 `esp-aes: Failed to allocate memory`，以及 Opus 编码后出现蓝牙队列或浮点日志崩溃。助手任务改为 64 KiB PSRAM 栈，实测首次编码的栈最低剩余为 41,592 字节，即已用约 24 KiB，超过原来的 16 KiB。USB 诊断改用已有的无驱动 VFS 轮询，不另分配中断驱动与收发环形缓冲区；Wi-Fi 静态 TX 缓冲从 8 个调整为 4 个，RX 缓冲保持 8 个，保留内部 RAM 给 WebSocket 栈、AES 和 DMA。语音上传日志每 100 帧记录栈水位与 DMA 余量，不记录音频内容。

修复版仅烧录 COM5 的应用分区，保留 NVS、Wi-Fi 与蓝牙配对。`logs/assistant-stack-fixed.log` 的 125 秒采集中收到服务器 hello 并连续上传超过 1,100 帧 Opus 音频，内部 RAM 保持 14,339 字节，未再出现 AES 分配失败、断言或重启。采集中有一次服务器主动发送空关闭帧，未提供关闭原因，因此不能宣称全程未断线；之后两次重新开始均成功连接。后续 `logs/assistant-connect-verified-status.log` 确认 `connected=1 listening=1 speaking=0 activation=none`，继续上传至 800 帧，栈最低剩余 38,088 字节。当前没有收到六位激活码；这并不能单独证明设备已绑定到某个账户。语音识别准确率、音乐试听与账号首次绑定仍需人工验收。

助手与音乐服务的任务栈显式分配到 PSRAM，保留内部 RAM 给蓝牙、DMA 和实时任务。首次实机烧录发现蓝牙初始化因内部 RAM 不足断言重启，因此不能仅用编译成功判断设备可启动。

2026-10-03 修复后在 COM5 重新烧录，连续采集 75 秒日志：未再次断言或重启，蓝牙初始化和电脑连接成功，Wi-Fi 自动恢复，桌面定时器正常运行，额度推送与设备状态 RPC 正常。日志为 `logs/assistant-boot-fixed.log`；这项启动验收不包含屏幕目视确认、语音识别和音乐试听。

原生单元测试：

```powershell
docker run --rm --mount "type=bind,source=$PWD,target=/work" espressif/idf:v5.5.3 bash /work/scripts/test-audio.sh
```

测试实际生产 MCP 分发器、共享音频输出与 HTTP 辅助逻辑，覆盖八个工具、参数与范围校验、未知工具、通知、执行失败、提示音优先级、中途抢占与节流、输入共存、重采样、音量、响应头、PCM 类型与 JSON 大小限制。

圆屏预览与隔离构建：

```powershell
./scripts/preview-ui.ps1
./scripts/build-audio.ps1
python scripts/verify_artifacts.py --dist dist/assistant --build .cache/assistant-build/firmware/build
```

`build-audio.ps1` 使用 `.cache/assistant-build/firmware` 与独立组件卷，输出到 `dist/assistant`，不烧录，不覆盖主构建产物。同一助手构建不要并行运行；标准 `build.ps1` 不要与另一个使用同一 `firmware/build` 或 `managed_components` 的构建同时运行。

生产助手/音乐页面的宿主测试可运行如下命令：

```powershell
New-Item -ItemType Directory .cache/assistant-preview -Force
docker run --rm --mount "type=bind,source=${PWD},target=/work" --mount 'type=volume,source=waveshare-assistant-components,target=/work/firmware/managed_components' --mount 'type=volume,source=waveshare-boot-ui-preview,target=/preview-build' espressif/idf:v5.5.3 bash -c 'cmake -S /work/tests/ui_preview -B /preview-build -DCMAKE_BUILD_TYPE=Debug && cmake --build /preview-build --target audio_preview -j 8 && /preview-build/audio_preview /work/.cache/assistant-preview'
```

该测试使用真实 LVGL 与生产 `audio_apps.cpp`、`codex_micro_app.cpp`，模拟 GPIO、时间和服务，覆盖两应用独立响应、后台不响应、按住跨页面切换、进入时先松开、700 毫秒长按、队列拒绝与释放兜底、暂停后关闭不重复发送，以及 100 次页面创建/关闭。音频单元测试还覆盖消抖、时间回绕、录音归属及连接期间松开。生成的 PPM 预览不会证明实际云端语音识别或 Brookesia 页面切换已成功。

2026-10-03 已在 COM5 实测耀狐网易点歌：签名请求返回歌曲元数据，FFmpeg 输出非静音的 16 kHz 单声道 PCM；设备 `/resolve` 与 `/stream` 均返回 HTTP 200，播放状态累计到 13.5 秒，暂停后返回小智并保持语音上传。桥接服务允许最长 30 分钟连接，避免长时间暂停被短连接超时截断。

实机验收清单：首次激活、识别与 TTS、自然语言点歌与暂停、断网恢复、切换 Codex Micro 后的完成通知，以及音乐/语音期间的提示音抢占。宿主测试通过不等同于所有真实环境均通过。
