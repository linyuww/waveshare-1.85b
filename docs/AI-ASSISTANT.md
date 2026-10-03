# 小智助手与本地 MCP

## 当前范围

保留 Brookesia 桌面、Codex Micro、健身应用、共享 Wi-Fi 与完成提示音，只安装一个「小智助手」图标。音乐播放器、歌曲输入框、音乐任务及音乐 MCP 工具已暂时移除，电脑伴侣不再自动启动音乐桥接。

移除前代码已提交至 `1e9d977440db298c04c64c49429d32782352096b`，固件备份在 `.cache/firmware-backups/1e9d977440db298c04c64c49429d32782352096b/`。旧音乐桥接脚本及私有配置保留以便后续恢复，但不属于当前设备运行流程。

交互对照 [小智源码](https://github.com/78/xiaozhi-esp32/blob/0d576d3d4c049c6f55eaf879725dc23e516511b4/main/application.cc) 的 `HandleToggleChatEvent()` 和 `tts/stop` 分支：通过现有板级音频、网络适配器接入桌面，而不是用完整上游固件替换 Brookesia。界面仅有头像、状态、当前一句字幕、必要的激活码，以及系统返回入口；不再展示开始、打断、说完、停止等多组虚拟按键。

- 使用上游 WebSocket v1、JSON hello/listen/STT/TTS/MCP 和 Opus 二进制音频。
- 依赖 `78/esp-opus 1.0.5`、`espressif/esp_websocket_client 1.7.0`，构建环境 ESP-IDF 5.5.3。
- 当前没有本地唤醒词、AEC、完整小智固件 OTA 或 MQTT 音频；不支持 WebSocket v2/v3 二进制帧。服务器需接受 v1。
- NVS 配置保持原有 1284 字节布局，旧音乐地址位置仅为保留字段，不参与运行；Wi-Fi、激活身份和令牌不因本次精简而丢失。

## 使用方式

1. 在桌面「设置」连接 Wi-Fi；小智使用现有 `SystemService` 网络，不再配一份 Wi-Fi。
2. 同页的「AI 助手与共享音频」可配置激活服务、可选自建 WebSocket/令牌和共享音量。默认激活地址为 `https://api.tenclass.net/xiaozhi/ota/`。
3. 打开「小智助手」，自动连接并进入自动断句的聆听模式。正常说话即可，不必按「说完了」。
4. 点头像切换对话：

| 当前状态 | 单击结果 |
| --- | --- |
| 待机 / 未连接 | 建立音频连接，开始聆听 |
| 正在连接 | 忽略重复点击 |
| 正在聆听 | 结束当前对话、关闭音频连接 |
| 小智正在说话 | 发送 abort，停止当前回复并恢复聆听 |

收到服务端 `tts/stop` 后自动恢复聆听。打断时按上游的 `aborted_` 逻辑丢弃剩余回复音频，等待服务端结束当前回复再收音，避免自行抢先启动下一轮。BOOT 仅在小智前台按住说话、松开发送，保留现有 WebSocket v1 与自动断句协议。暂停、返回桌面或切换应用时，只结束本次 BOOT 操作拥有的录音，不关闭头像/控制台启动的后台会话；服务端自动结束该轮时也释放其归属。进入时已按住 BOOT，必须先松开再按下。连接或打断尚未完成就松开时，已开始的操作在通道就绪后结束；尚未执行的按下请求直接丢弃。Codex Micro 的蓝牙额度同步与提示音保持独立。

服务端要求绑定时，六位激活码单独显示在头像下方。在小智账户的设备激活页面完成绑定，再点头像重连。正常 hello 没有返回激活码时不会编造一个验证码，这也不能单独证明设备属于哪个账户。自建服务填写地址后保存，关闭再打开助手生效；非 TLS 的 `ws://` / `http://` 仅用于可信局域网。

## 音频

`shared_audio` 唯一创建 ES8311 / ES7210 和 I2S 驱动，避免重复初始化。硬件采样率为 24 kHz，麦克风转换到 16 kHz 后编码为 Opus。无 AEC 时小智说话期间不上传麦克风音频，回复结束后继续上传。

Codex 任务完成提示音保持最高优先级，抢占小智输出。音量为 0 或原有提示音开关关闭时仍尊重静音配置。助手任务栈 64 KiB，显式分配到 PSRAM；GUI 栈保持 16 KiB，每分钟日志包含 `UI-stack-free`，用于追踪长时间停留问题。

## 本地 MCP

实际工具选择由小智服务端完成，设备只执行严格校验过的请求；参数不可执行任意 shell。

| 工具 | 参数 | 作用 |
| --- | --- | --- |
| `self.codex.open` | 无 | 打开 Codex Micro |
| `self.apps.open` | `app`: `codex` / `assistant` / `settings` | 打开设备应用 |
| `self.audio.set_volume` | 整数 `volume`，0–100 | 设置并保存共享音量 |
| `self.device.get_status` | 无 | 查询 Wi-Fi、助手连接、音量与提示音优先状态 |

`self.music.*` 不再发布或执行，`self.apps.open` 也不接受 `music`。新建连接会重新协商工具清单；服务器中旧的音乐提示词不能使设备恢复播放器。

## 调试与验证

USB 串口支持 `xiaozhi status|connect|toggle|stop|open` 和 `help`。例如：

```powershell
& D:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe scripts/assistant-console.py --port COM5 --command open --seconds 30 --output logs/assistant-simple.log
```

状态日志不输出令牌或 Wi-Fi 密码。已保留的小写 Device-Id 修复、64 KiB Opus 栈修复和收音/播音切换修复不回退；历史记录见 `logs/assistant-connect-verified-status.log` 和 `logs/assistant-stack-fixed.log`。

宿主测试运行实际 MCP 分发器、共享音频与 HTTP 辅助逻辑，覆盖四个工具、移除音乐工具、输入校验、提示音抢占、原版式单键状态转换与设备身份。UI 测试编译实际 `audio_apps.cpp`、图标和中文字库，验证 100 次助手/设置开关、BOOT 按住/释放与前台切换、激活码展示、配置布局兼容、页面销毁，以及 10000 次无变化刷新。

```powershell
docker run --rm --mount "type=bind,source=$($PWD.Path),target=/workspace" -w /workspace espressif/idf:v5.5.3 bash -c "cmake -S tests/audio -B .cache/audio-tests && cmake --build .cache/audio-tests && ctest --test-dir .cache/audio-tests --output-on-failure"
docker run --rm --mount "type=bind,source=$($PWD.Path),target=/workspace" -w /workspace espressif/idf:v5.5.3 bash -c "cmake -S tests/ui_preview -B .cache/audio-ui-build && cmake --build .cache/audio-ui-build --target audio_preview && mkdir -p .cache/assistant-preview && .cache/audio-ui-build/audio_preview .cache/assistant-preview"
./scripts/build-audio.ps1
python scripts/verify_artifacts.py --dist dist/assistant --build .cache/assistant-build/firmware/build
```

宿主渲染在 `.cache/assistant-preview/assistant.ppm` 和 `activation.ppm`，不是屏幕实拍。实机仍需人工验收识别、TTS、说话中打断，以及 Codex 完成时实际可听到的提示音。升级只写 COM5 的 `0x10000` 应用分区，不擦除 NVS 或重新配对。

### 2026-10-03 本次验证

- 四项宿主测试、100 次助手/设置生命周期与 10000 次空闲刷新通过；缩小应用内容区后仍能完整显示单键提示。桌面预览只含七个应用。
- 固件构建、分区容量、SHA256 和合并偏移校验通过；链接后的 ELF 不包含 `MusicService` 或音乐控制台实现。
- COM5 仅更新应用分区，写入 6429744 字节并通过设备哈希校验。移除前固件备份未覆盖。
- `logs/assistant-simple-controls.log` 记录 72 秒实机检查：Wi-Fi 和校时正常、进入助手后自动连接、单键结束后 `connected=0 listening=0 speaking=0`、再单击成功重连并恢复聆听，第二次连接连续上传至少 600 帧 Opus。
- 这段检查未见 panic 或重启；内部 RAM 在聆听时为 15923 字节，结束时恢复至 22955 字节，GUI 栈最低剩余 8432 字节。`music status` 已返回未知命令。
- 本次自动检查未注入真实语音，因此不把它当作 STT/TTS 或可听提示音的人工试听验收；预览中的 `123456` 是激活界面测试数据，不是设备实际验证码。
