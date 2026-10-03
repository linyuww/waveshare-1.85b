# Codex Micro 额度同步

先在统一设置中开启蓝牙，并在 Windows 中配对 **Codex Micro**。安装 Python 3.10+、PowerShell 7，并确保本机 Codex CLI 已登录。

双击 `start-companion.cmd` 可启动原项目的额度同步程序。它自动查找板子地址，每 60 秒同步额度；Ctrl+C 停止。

如需指定 Python、Codex 命令位置或设备地址，可编辑 `config.psd1`。不要在新旧项目里同时运行两份伴生程序。

只启动一次同步：

```powershell
./scripts/companion/start-companion.ps1 -Once
```

本目录与 `scripts/windows/` 中的后端复制自用户提供的 Codex Micro 项目。集成过程没有运行程序、设置自启动、删除电脑配对或修改系统蓝牙设置。
