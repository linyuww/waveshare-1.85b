# Codex Micro 本机网桥与额度同步

日常登录 Windows 后自动在后台运行，不需要一直打开终端。首次安装或迁移后执行下方安装命令。也可双击本目录的 `start-companion.cmd`：启动窗口很快关闭，后台程序继续运行。启动器先校验并启动本项目附带的网桥，再查找已配对的 **Codex Micro**，每 60 秒通过 BLE 同步真实额度。重复启动不会产生第二份后台同步程序。

## 后台与自启动

在项目根目录的 PowerShell 7.4+ 中执行：

```powershell
./scripts/companion/background.ps1 -Action Install
./scripts/companion/background.ps1 -Action Start
./scripts/companion/background.ps1 -Action Status
./scripts/companion/background.ps1 -Action Stop
./scripts/companion/background.ps1 -Action Disable
```

`Install` 注册当前用户登录后延迟 20 秒运行的计划任务；不是登录前的系统服务。使用普通用户权限，不保存密码。`Start` 立即后台启动（未安装时自动安装）；`Stop` 停止本项目后台进程树及网桥，但保留下次登录自启动；`Disable` 停止并关闭自启动。再次 `Start` 会重新启用自启动。

从旧版迁移：如果旧任务的修改权限被拒绝，需要仅在安装这一步打开管理员 PowerShell 7，并执行 `./scripts/companion/background.ps1 -Action Install -MigrateLegacy`。安装器导出旧任务 XML 备份后更新同名同步任务，并禁用已识别的旧网桥任务；后台任务本身仍以普通用户权限运行。旧任务未迁移时，`Start` 只启动本次登录的静默程序并提示警告，不会谎报新自启动已生效。若旧同步正在运行，应先在旧入口停止，再迁移。

计划任务使用 `pythonw.exe` 托管隐藏 PowerShell，BLE 子进程也不创建控制台窗口。后台日志为 `logs/companion/background.log`；蓝牙未就绪、开发板关机或网络中断时按原启动器规则退避重试。注销或关机会结束用户会话，休眠期间不会同步。保持本项目路径不变；移动文件夹后先在旧位置 `Disable`，再在新位置 `Install`。若同名任务属于旧安装，安装器会拒绝覆盖；请先导出旧任务备份，再迁移。

手动诊断仍可运行 `start-companion.ps1 -Once` 或 `-Probe`，但应先 `Stop`，避免与后台争用蓝牙。

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

`/quota` 缓存到期后可能同步请求账户接口（接口超时 15 秒），因此电脑读取超时设为 20 秒。持续同步时，临时额度读取失败会每 10 秒重试；蓝牙写入失败后的每次重试都会重新获取额度和重置倒计时，避免发送重试前的旧快照。正常同步间隔仍为 60 秒，账户页面与开发板可能在下一轮刷新前短暂不同。

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

日志位于 `logs/companion/companion-YYYY-MM-DD.log` 和 `logs/companion/bridge-stderr.log`。自启动安装后重启并登录即可自动恢复；不需要重新刷固件或配对。

## 自动测试

```powershell
python -m unittest discover -s tests -p test_windows_companion.py -v
```

测试覆盖额度转换、窗口顺序、新鲜度、代理隔离、异常数据、旧 CLI 参数拒绝，以及网桥程序和来源文件的校验；不会连接蓝牙或读取登录凭据。源码及二进制的来源与重新编译说明见 `scripts/bridge/README.md`。
