# 本机网桥迁移验证

验证日期：2026-10-03（Asia/Shanghai）。分支：`fix/codex-quota-local-bridge`。

## 文件来源

网桥程序和两个 Rust crate 从用户提供的 `codex-quota-widget` 目录复制。来源提交、工作树状态和 SHA-256 记录在 `scripts/bridge/runtime.json`。没有复制登录凭据、环境密钥、原目录日志或缓存。

## 真实运行结果

- 明确停止原目录路径下的旧网桥进程，未按名称批量停止其他程序。
- 从本项目 `scripts/bridge/bin/codex-ornament-bridge.exe` 启动新进程，实际监听 `127.0.0.1:8787`。
- `/ready` 返回 `ok=true`、`eventLogWritable=true`。
- `/quota` 返回可解析且新鲜的真实额度，电脑端后端读取成功。
- `start-companion.ps1 -Once` 自动复用本项目网桥，向已配对的 Codex Micro 写入，第一次尝试即收到 `write_result=Success` 和 `write_ack=Success`。
- 连续同步测试从 12:24:40 +08:00 开始，完成三轮 ATT 成功确认，之后停止测试用同步进程及其 Python 子进程；本项目网桥保留运行。

测试没有修改固件、重新刷机、删除蓝牙配对或设置开机自启动。日志保存在被 Git 忽略的本机 `logs/companion/`。

## 自动检查与限制

```powershell
python -m unittest discover -s tests -p test_windows_companion.py -v
python -m py_compile scripts/windows/windows_companion.py tests/test_windows_companion.py
```

25 项 Python 测试通过，包括网桥二进制与源码校验。PowerShell 启动脚本解析检查通过。

Rust 源码构建尝试因本机缺少 MSVC `link.exe` 失败，Rust 测试没有运行。本次采用完整复制并校验的既有 Windows 程序，运行结论来自真实重启和 BLE 写入，不将预编译程序运行成功等同于源码构建成功。
# 静默后台补充验证（2026-10-03）

- 新增 `background.ps1` 和 `background_companion.py`：当前用户登录计划任务、pythonw 隐藏托管、进程互斥、停止进程树和禁用入口。
- 两次后台启动后，仅存在一份本项目 pythonw 主进程；PowerShell、Python 和网桥进程窗口句柄均为 0。
- 已实际执行停止后重新启动；新网桥 `/ready` 正常，BLE 日志出现 `write_result=Success` 和 `write_ack=Success`。
- 27 项 companion Python 单元测试通过，PowerShell 管理脚本语法检查通过。
- 本机旧 `Codex Ornament Bridge` 任务已备份并禁用。旧 `CodexMicroAllowanceCompanion` 任务的注销和禁用均被系统拒绝访问，仍指向旧目录。因此当前仅验证本次会话静默运行，不能声称新的登录自启动已生效，也未执行整机重启验证。
- 完成迁移需在管理员 PowerShell 7 中运行 `./scripts/companion/background.ps1 -Action Install -MigrateLegacy`；任务运行权限仍为 Limited。之后用普通 PowerShell 执行 `-Action Start`。

## 管理员迁移后复验（2026-10-03）

用户执行管理员迁移后，已重新读取计划任务：入口为本项目 `background_companion.py`，使用 `C:\Python312\pythonw.exe`；当前用户登录触发，延迟 20 秒，Interactive/Limited 权限，无运行时长限制，重复实例 IgnoreNew。旧网桥任务 Disabled。

停止本次会话原后台进程后，已通过普通用户执行 `background.ps1 -Action Start` 启动计划任务。任务状态 Running，新后台日志收到 BLE `write_result=Success`、`write_ack=Success`。持续运行任务的 LastTaskResult 为 267009（0x41301，正在运行），并非失败。未执行整机重启/重新登录验证。

任务注册、启用、启动、停止和禁用现在显式使用 `-ErrorAction Stop`，避免操作被拒绝时继续输出成功提示；已启用任务不再做不必要的启用写操作。
