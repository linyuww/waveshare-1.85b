# HID 与 HTTP 配额

固件 0.3.3 使用 ESP-IDF 5.5.3 的 NimBLE，沿用 Muse 的主机初始化、
NVS 配对存储、重复配对恢复与 GAP 事件处理方式。HID 使用适合电脑的
Just Works 配对，保留 VID 303A、PID 8360、报告 ID 6 和 63 字节报告体。
蓝牙提供 Device Information、HID 和 Battery 服务，负责控制及线程状态 RPC。
通知尊重客户端订阅，移除预置 CCCD 和每秒电池保活通知。

Windows Realtek 适配器联调后，默认 PHY 固定为 1M，ATT MTU 为 128，
仍使用加密的 Secure Connections / Just Works 配对。MTU 能容纳完整的
63 字节 HID 报告，不需要 517 字节的协商值。广播和 GAP 服务均声明
Generic HID 外观。加密、订阅、MTU 等早于 CONNECT 到达时先建立会话，
CONNECT 不再清空已恢复的加密和订阅状态；广播失败最多每秒重试一次。

配额由独立 `QuotaService` 负责：

```
开发板连接电脑的 2.4 GHz 热点
  → 读取 DHCP 默认网关
  → GET http://<网关>:8787/quota
  → 校验数据、发布快照、更新屏幕
```

成功后每 60 秒刷新，失败后 10 秒重试，单次请求最多 22 秒。
关闭蓝牙、离开 Codex 界面或 HID 断开都不会停止 HTTP 配额服务。
错误响应保留最近成功的值；超过 180 秒显示过期。重启后等待首次 HTTP 数据。
电脑计算倒计时，设备不依赖 SNTP。切换热点后重新读取网关。

## 电脑端

PowerShell 7.4：

```powershell
./scripts/quota-server.ps1 -Action Start
./scripts/quota-server.ps1 -Action Status
./scripts/quota-server.ps1 -Action Stop
```

启动器将内置认证网桥绑定到 `127.0.0.1:8786`，新 Python 服务在 8787
提供唯一的只读 `/quota`，只返回剩余百分比、倒计时和采样年龄。认证文件、
令牌以及原网桥其他接口不向热点开放。网桥二进制启动前校验 SHA-256。

Windows 热点通常使用 `192.168.137.1`；设备自动使用网关，不硬编码该地址。
Windows 防火墙需允许当前 Python 程序的 TCP 8787 入站，建议范围为 LocalSubnet。
需要登录自启时，在管理员 PowerShell 运行 `./scripts/quota-server.ps1 -Action Install`。
新任务名称为 `CodexMicroQuotaHttp`。
`Install` 同时删除本项目旧的蓝牙自启动任务，并添加仅允许本地子网的 8787 防火墙规则。

升级时删除旧的 `CodexMicroAllowanceCompanion` 计划任务。本次会话已停止它，
如普通用户删除被 Windows 拒绝，管理员可执行：

```powershell
Unregister-ScheduledTask -TaskName CodexMicroAllowanceCompanion -Confirm:$false
```

源树已删除蓝牙配额服务、配额写队列、NVS 配额缓存、蓝牙写入/探测脚本及其测试。
HTTP 是唯一的配额通道，没有旧协议回退分支。历史版本的验证文档只记录历史事实。

## 验证

`test_quota_server.py` 验证窗口按时长识别、时区、过期/无效数据、HTTP 错误
与接口范围；`quota_http_test` 验证固件解析与关闭蓝牙后的配额有效性。
实际稳定性仍需在设备连接电脑热点并重新配对 HID 后，通过持续日志确认。
USB 控制台命令 `quota` 可以查看热点网关、HTTP 状态、采样年龄和当前配额。
`ble status` 查看连接与主机 RPC 状态，`ble pair` 清空设备端配对密钥并重新广播，
`ble on` / `ble off` 通过蓝牙服务任务开关蓝牙。重新配对时也应先移除电脑端记录。

安装可选的 `hidapi` 后，运行 `python scripts/verify_hid.py --seconds 120`，
通过 Windows HID 驱动持续读写 `sys.version` / `device.status`，检查每次
请求对应的真实响应。该脚本只读取设备状态，不发送控制键，不参与配额获取。

### 2026-10-08 设备联调（0.3.3）

- COM5 实际烧录并验证 SHA-256，保留 NVS；Windows Realtek 适配器成功配对，
  系统枚举 VID 303A / PID 8360 / Usage Page FF00 的 HID 接口。
- 去掉临时 ATT/HCI 跟踪后重启，约 3.7 秒自动恢复加密和输入/电池订阅，
  Codex 持续发送 `v.oai.rgbcfg`、`v.oai.thstatus` 和 `device.status`。
- 真实 Windows HID 驱动上连续 120 秒、60 次版本/状态请求全部收到对应响应；
  重启后的 180 秒日志未出现断联或崩溃。
- 同时连接电脑热点，启动、约 60 秒、约 120 秒均成功通过 HTTP 刷新配额。
- 固件构建、6 项主机测试及 0.3.3 分发产物校验通过。
