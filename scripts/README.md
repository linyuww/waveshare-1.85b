# 脚本入口

## 配额服务

日常只运行 `quota-server.ps1`，需要 PowerShell 7.4 和 Python 3.12；Python 服务只使用标准库，不需要安装 pip 包或 Rust。

```powershell
# 在项目根目录启动、查看配额、停止
./scripts/quota-server.ps1 -Action Start
./scripts/quota-server.ps1 -Action Status
./scripts/quota-server.ps1 -Action Stop

# 管理员 PowerShell：设置登录自启和本地子网防火墙规则
./scripts/quota-server.ps1 -Action Install
```

默认 Python 为 `D:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`。其他安装路径用 `-Python 'C:\path\python.exe'` 指定；`Install` 会保存该路径供登录自启使用。`Start -Foreground` 用于前台观察和计划任务。

| 文件 | 当前用途 |
| --- | --- |
| `quota-server.ps1` | 唯一管理入口：校验网桥、启动、停止、查看状态和安装自启 |
| `start-quota-server.ps1` | 当前已安装 Windows 任务仍使用的入口，仅转发至管理脚本，没有独立启动逻辑 |
| `quota_server.py` | 在 TCP 8787 提供只读 `GET /quota`，校验并转换配额数据 |
| `bridge/bin/codex-ornament-bridge.exe` | 从电脑上的 Codex 登录状态读取配额，仅监听 `127.0.0.1:8786` |
| `bridge/runtime.json` | 网桥来源及 SHA-256；启动前校验程序完整性 |
| `bridge/Cargo.*`、`bridge/crates/` | 认证网桥源码、依赖锁定及复现材料，日常无需编译 |

电脑开启 2.4 GHz 热点，开发板连接热点后定时请求 `http://<热点网关>:8787/quota`。蓝牙只负责 HID 控制，不参与配额传输。电脑需已登录 Codex；`Status` 报错时检查登录状态及 `logs/quota-http/`。完整部署和旧计划任务清理说明见 [HID 与 HTTP 配额](../docs/HID-HTTP-QUOTA.md)。

验证：`python -m unittest discover -s tests -p test_quota_server.py -v`。

## 当前开发工具

以下脚本用于仍然存在的固件功能或开发流程，不是配额后台服务：

| 工具 | 用途 |
| --- | --- |
| `build.ps1`、`flash.ps1`、`verify_artifacts.py` | 统一固件构建、保留 NVS 的烧录、分发产物校验；助手也使用同一构建入口 |
| `_env.ps1`、`_host-env.ps1` | 其他脚本加载的 ESP-IDF 和宿主编译环境 |
| `monitor.py`、`assistant-console.py`、`verify_hid.py` | USB 日志、小智串口命令和真实 HID 连续验证 |
| `preview-ui.ps1`、`preview*_images.py` | 实际 LVGL 桌面、设置和健身界面的圆屏预览 |
| `generate_icons.py`、`generate_font.ps1` | 桌面图标和中文字库生成 |
| `generate_fitness_assets.py`、`verify_fitness_assets.py` | 健身动画资源生成与完整性验证 |
| `test-audio.*`、`test-fitness.*`、`test-render.*` | 现有功能的 Windows / Linux 宿主测试 |

已删除停用的音乐桥接服务、启动器、配置示例、对应测试，以及助手独立构建脚本。配额启动逻辑已合并进唯一管理入口；旧入口仅供已安装的计划任务转发，重新运行 `Install` 会让任务直接使用管理脚本。当前没有蓝牙配额脚本。历史验证文档中的旧命令仅记录当时结果。
