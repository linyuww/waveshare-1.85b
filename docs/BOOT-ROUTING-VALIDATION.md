# BOOT 前台路由验证

日期：2026-10-03。基线：`main` 的 `ecb5fd9`，开发分支：`codex/fix-boot-app-routing`。

## 修改范围

- 小智补齐暂停、恢复和关闭生命周期，Codex 与小智均检查前台状态及当前屏幕。
- 共用 30 毫秒消抖，按住进入页面时先松开再按下。Codex 保留短按 Voice Chat 与 700 毫秒长按 Mic。
- 小智 BOOT 请求使用会话编号；服务任务只结束对应录音，结束标记不经过普通命令队列。其他按钮或后台操作接管会话后，旧 BOOT 松开不会结束新会话。
- Codex 成功入队后才进入 Mic 按住状态，释放入队失败时使用现有可靠释放机制；暂停后关闭不重复释放，关闭时销毁定时器。
- 未修改蓝牙服务、蓝牙协议、小智消息格式、板卡引脚或配置分区。

## 已通过的验证

1. `espressif/idf:v5.5.3` 中执行 `bash scripts/test-audio.sh`，5/5 通过：MCP、音频优先级、HTTP 辅助函数、小智协议和新增 BOOT 状态测试。新增测试覆盖按下/松开消抖、时间回绕、按住进入保护、700 毫秒边界、请求失败重试、结束去重、录音归属和连接等待期间松开。
2. 按 `AI-ASSISTANT.md` 中的 Docker 命令构建并运行 `audio_preview`。测试使用真实 LVGL 和生产 Codex/音频页面源码，服务与 GPIO 为模拟。验证两应用独立响应、后台不响应、按住跨页面切换、松开后重新按下、音乐页面不响应、请求拒绝与释放兜底、小智离开结束一次、Codex 长按离开释放一次、暂停后关闭不重复操作及 100 次音频页面清理。
3. 运行 `scripts/build-audio.ps1` 完整构建，再使用最终源码重跑构建及固件合并。最终构建退出码 0；构建副本全部 `firmware/main` 源码的 SHA256 与开发分支一致。最终重建无编译警告或错误。首次配置曾出现现有可选依赖的 Kconfig 提示，未阻止构建。
4. `python scripts/verify_artifacts.py --dist dist/assistant --build .cache/assistant-build/firmware/build` 通过：版本 `0.3.0`、全部 SHA256、合并偏移、NVS 空白区及应用分区容量。`git diff --check` 通过。

## 固件

- 应用镜像：6,465,552 字节，8 MiB 应用分区剩余约 23%。
- 合并镜像：6,531,088 字节。
- 合并镜像 SHA256：`93B03D1D49293C3ACEFAF545DAC77AD88D2D9B0EA9D0EAFD57F6148E39548A8F`。
- 构建输出位于该 main 工作区的 `dist/assistant/`；交付副本位于主项目目录 `dist/boot-routing/`，包含合并镜像、bootloader、分区表、应用镜像及 SHA256 清单。

## 未执行的验证

此次未烧录设备、未执行实机 BOOT/页面切换/云端语音验证，未发布 Release。宿主模拟测试和固件编译通过不能视为实机通过。仍需在实际设备上验证按键电气消抖、Brookesia 页面切换、长按释放及小智结束消息的云端效果。
