# Codex Micro 本机网桥与额度同步

日常只需双击本目录的 `start-companion.cmd`。启动器先校验并启动本项目附带的网桥，再查找已配对的 **Codex Micro**，每 60 秒通过 BLE 同步真实额度。已运行的本项目网桥会被复用；同步窗口保持打开，Ctrl+C 停止 BLE 同步，网桥继续在后台运行。

## 环境

- Windows x64、Python 3.10+、PowerShell 7.4+。
- 开发板已开机、开启蓝牙，并在 Windows 中配对 **Codex Micro**。
- 本机 Codex 已登录；网桥沿用本机登录信息，不要求填写 API key。
- 日常使用无需安装 Rust，也不需要 `D:\Desktop\codex` 原目录。

## 数据链路与文件

```text
本项目 scripts/bridge/bin/codex-ornament-bridge.exe
  -> http://127.0.0.1:8787/quota
  -> scripts/windows/windows_companion.py
  -> BLE -> 开发板
```

`scripts/bridge/` 包含从原项目复制的 Windows 网桥程序、两个 Rust crate、Cargo 工作区和来源校验记录。旧的 CLI App Server 额度读取代码及 `--no-bridge` / `--codex-path` 已移除。Python 后端只负责额度格式校验与 BLE 传输，固件协议没有改变。

默认网桥只监听本机 `127.0.0.1:8787`。启动前检查可执行程序 SHA-256，发现其他程序占用端口时会报错，不会杀掉未知进程。迁移时需先停止旧目录运行的网桥，不要同时运行两份 BLE 同步程序。

网桥使用本机登录文件，不复制 `.env`、登录凭据或 token 到项目。`CODEX_HOME` / `CODEX_AUTH_PATH` 的环境变量仍按原网桥规则生效。额度缓存、事件记录和运行日志写入本项目的 `logs/companion/`，不会被 Git 提交。

## 配置和诊断

`config.psd1` 可设置 `DeviceAddress`、`PythonPath`、`IntervalSeconds`、`BridgeUrl`、`BridgeMaxAgeSeconds` 和 `AutoStartBridge`。默认 `AutoStartBridge = $true`。如需连接手动管理的网桥，设为 `$false`；自动启动仅接受本机 HTTP `/quota` 地址。

网桥快照以 `capturedAt` 判断新鲜度，默认最多 180 秒。连接失败、额度窗口缺失或快照过期时，不会写入旧额度，也不会回退到 CLI。启动器会退避重试，恢复后重新同步。

仅启动网桥，不连接蓝牙：

```powershell
./scripts/companion/start-bridge.ps1
```

仅检查额度，不写开发板：

```powershell
python ./scripts/windows/windows_companion.py --json-only -v
```

启动网桥并同步一次：

```powershell
./scripts/companion/start-companion.ps1 -Once
```

日志位于 `logs/companion/companion-YYYY-MM-DD.log` 和 `logs/companion/bridge-stderr.log`。目前没有配置开机自启动，电脑重启后重新双击入口即可；不需要重新刷固件或配对。

## 自动测试

```powershell
python -m unittest discover -s tests -p test_windows_companion.py -v
```

测试覆盖额度转换、窗口顺序、新鲜度、代理隔离、异常数据、旧 CLI 参数拒绝，以及网桥程序和来源文件的校验；不会连接蓝牙或读取登录凭据。源码及二进制的来源与重新编译说明见 `scripts/bridge/README.md`。
