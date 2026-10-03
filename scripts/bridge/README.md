# 本项目内置的 Codex 网桥

从用户提供的 `D:\Desktop\codex\codex-quota-widget` 复制，供本项目独立运行。日常入口是 `scripts/companion/start-companion.cmd`，不需要安装 Rust。

## 内容与来源

- `bin/codex-ornament-bridge.exe`：原项目正在使用的 Windows x64 可执行程序，完整复制，大小 9,135,616 字节。
- `crates/codex-ornament-bridge/`：原目录网桥源码及依赖声明。
- `crates/quota-core/`：原目录额度读取核心源码及依赖声明。
- `Cargo.toml` / `Cargo.lock`：只包含上述两个 crate 的独立工作区与依赖锁定。
- `runtime.json`：来源 Git HEAD、导入日期、程序及源码 SHA-256。原目录工作树存在未提交修改，因此来源记录不声称是干净提交。

二进制为原项目已编译的现有程序，本次没有重新构建它。源码是原目录当前工作树的快照，不能据此保证与旧二进制严格一一对应。二进制按原始字节记录校验值，源码按 LF 换行记录校验值，避免 Git 换行转换影响校验；启动器会校验二进制，自动测试会校验两类文件。

只复制程序与源码，没有复制 `.env`、`auth.json`、token、原目录日志或缓存。

## 运行约束

本项目启动器默认将网桥绑定到 `127.0.0.1:8787`，并将额度缓存与事件日志隔离到 `logs/companion/`。登录信息按原程序逻辑从本机 Codex 登录文件读取；不会发送到开发板，也不会存入项目。

完整原程序还包含 hook、天气、音乐等接口。本项目只依赖 `/ready` 和 `/quota`，没有复制原项目的天气或音乐密钥配置。

## 从源码重新编译

需要 Rust MSVC 工具链，以及 Visual Studio / Build Tools 的 C++ 编译环境和 Windows SDK。在相应 Developer PowerShell 中执行：

```powershell
cargo build --locked --release --manifest-path scripts/bridge/Cargo.toml -p codex-ornament-bridge
cargo test --locked --manifest-path scripts/bridge/Cargo.toml
```

构建输出位于被 Git 忽略的 `scripts/bridge/target/`。替换内置程序时，要先停止该程序，再复制构建结果并同步更新 `runtime.json` 的程序 SHA-256、大小及来源，之后重跑自动测试和 BLE 写入验证。

本次机器缺少 `link.exe`，源码构建在 MSVC 链接阶段失败；没有安装或修改系统构建工具。运行可行性以已复制程序的真实重启、HTTP readiness、额度读取及 BLE ATT 确认为准，而不是声称源码构建或 Rust 测试已通过。
