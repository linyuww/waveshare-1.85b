# BOOT 前台路由验证（2026-10-03）

本次以本地 main 的 `5f57b40` 为基线，保留精简小智界面、四个 MCP 工具和音乐功能移除。旧同名分支已移除，新工作分支为 `codex/fix-boot-app-routing`。

## 行为

- BOOT 只响应当前前台应用及其活动屏幕。桌面和其他应用不响应。
- 两应用共用 30 毫秒消抖；进入时已按住，需要先稳定松开再按下。
- Codex 保留短按 Voice 和 700 毫秒长按输入。离开只释放控制；长按入队失败不设置已按住状态，松开入队失败使用原有可靠释放通道。蓝牙实现与协议未修改。
- 小智按下提交带标识的收音请求，松开或离开提交该标识的结束请求。结束信号独立于命令队列；未入队的按下、已释放但尚未执行的按下不会结束后台会话。暂停后关闭不会重复结束。
- 连接/打断进行中释放时，已受理的录音在通道就绪后结束。服务端开始正常回复、头像或控制台取得会话控制权时，原 BOOT 归属失效。继续使用现有自动断句、WebSocket v1、Opus 与 JSON 消息格式。
- 返回桌面保留头像/控制台启动的后台会话，只结束当前 BOOT 录音。

## 已执行

- Docker `espressif/idf:v5.5.3`：`idf.py --version` 确认 5.5.3。
- `tests/audio` 的 5 项测试通过：MCP、共享音频优先级、HTTP、现有小智协议、BOOT 控制状态机。BOOT 测试覆盖消抖、时间回绕、进入保护、700 毫秒阈值、失败重试和录音归属。
- `audio_preview` 编译实际 `audio_apps.cpp` 和 `codex_micro_app.cpp`，使用宿主 LVGL 和服务测试桩：100 次应用/设置生命周期、10000 次空闲刷新、两个应用独立响应、后台不响应、跨界面按住、松开后重新按下、拒绝入队、离开时结束一次、Codex 长按离开时释放一次，以及暂停后关闭不重复发送均通过。
- 宿主预览 `.cache/boot-preview/assistant.png` 已目视检查，提示文字完整；预览不是实机照片。
- `scripts/build-audio.ps1` 完整固件构建、合并通过。应用分区镜像 6,430,896 字节，小于 8 MiB；`scripts/verify_artifacts.py` 的版本、SHA256、合并偏移、NVS 填充和容量检查通过。
- 产物：`dist/assistant/waveshare_launcher.bin`（应用）和 `dist/assistant/waveshare-launcher-usb.bin`（合并镜像）。
- 远端 main 原为旧功能线 `88da770`；依用户“一切以新的 main 为主”的要求衔接其历史，保持本次基于本地新 main 的文件内容，正常推送，不强制覆盖远端历史。

## USB 烧录与实机冒烟测试（2026-10-03）

- 用户追加授权烧录测试后，通过 COM5 写入 `a04f40c` 所对应应用固件，仅写 `0x10000`，保留 NVS。esptool 4.11.0 写入 6,430,896 字节，设备端哈希校验通过；日志 `logs/boot-routing-flash.log`。
- 复位观察 45 秒：启动版本 0.3.0，ELF SHA256 前缀 `9ea212b88` 与导出镜像一致。Wi-Fi 自动连接、校时成功，HTTPS 探测 `ESP_OK / HTTP 302`，证书校验成功；日志 `logs/boot-routing-hardware-boot.log`。
- 随后观察 58 秒，通过现有 USB 控制台打开小智、查询状态、toggle 停止、toggle 重连，再 stop。两次 hello 成功，采样率 24 kHz；两次均记录至少 300 帧 Opus 上行。聆听状态均为 `connected=1 listening=1 speaking=0`，停止后均为 `connected=0 listening=0 speaking=0`；日志 `logs/boot-routing-hardware-assistant.log`。
- 该段日志未见 panic、看门狗、栈溢出、内存分配失败或非预期重启。小智任务栈剩余约 41.6 KiB，GUI 栈剩余 8144 字节；停止后内部可用 RAM 恢复至 22955 字节。
- 观察到 BLE 反复断开并自动重连，原因码 `0x08`；配对仍保留，但 BLE 稳定性不算通过。本次不修改蓝牙实现，尚未定位这一现象。

## 尚未验收

未发布 Release。实体 BOOT 按住/松开、按住时触摸跨应用切换、真实 STT/TTS 和可听音频效果仍需人工验收；USB 控制台启停与上行帧计数不能替代这些操作的实机验证。
