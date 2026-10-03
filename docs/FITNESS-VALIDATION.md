# 健身应用交付验证

验证日期：2026-10-03。功能版本：0.3.0。目标：Waveshare ESP32-S3 Touch LCD 1.85B，360×360 圆屏。

## 分支和源码范围

- 分支：`codex/fitness-round-screen`；基线提交：`de25c8d74e92386d33fb8db0395f6ecfde280d1a`。
- 独立 Git worktree：`D:\Desktop\waveshare-1.85b\.cache\fitness-worktree`。本次改动保留在该分支的工作区，未创建提交。
- 原主工作区有其他任务同时开发并切换分支，因此没有再切换、撤销或提交其改动。
- 固件构建使用 `.cache\fitness-build\firmware` 独立源码快照，内容为上述基线加健身功能，不包含并发任务的语音、音乐或其他新功能。受管理依赖沿用项目已有版本。
- 发布文件位于独立 worktree 的 `dist/fitness/`；不替换主工作区 `dist/` 的其他固件。

## 自动验证结果

| 范围 | 结果 |
| --- | --- |
| 状态机宿主测试 | 通过，C++17、`-Wall -Wextra -Werror`、ASan/UBSan 与泄漏检查 |
| 记组与休息 | 快速重复记组被拒绝；暂停、继续、跳过、到期；倒计时刷新不写存储 |
| 撤销与切换 | 撤销最近一组；跨刚切换动作撤销；完整遍历五个默认计划 |
| 训练结束 | 提前结束保留部分进度；最后动作最后一组直接生成总结 |
| 设置快照 | 组数、目标次数、休息、模板修改只影响新训练；无效值与重复动作被拒绝 |
| 持久化 | 重启后休息暂停；失败保留内存进度并支持重试；907 字节记录的全部截断与位损坏被拒绝 |
| 历史轮换 | 连续 35 次训练后正确保留最新 30 次；未校时不生成日期 |
| 原有 Codex 渲染回归 | 通过宿主 ASan/UBSan 检查与 50 次画布重开 |
| GIF 解码 | 14 个 GIF 均逐帧解码、循环重播，删除后解码定时器恢复基线 |
| 生产页面预览 | 使用真实 `fitness_view.cpp` 与 LVGL 9.5.0；检查中文字符、大按钮、圆屏边界、实际点击事件 |
| 故障页面 | GIF 失败使用静态示意；保存失败显示提示、可继续操作并重试 |
| 生命周期 | 100 次训练及媒体创建/释放后 LVGL 堆基线保持 16,520 字节，定时器数量不增长 |

16,520 字节是宿主 LVGL 堆基线，不是 ESP32 全系统 RAM/PSRAM 占用，也不代表实机内存验证完成。

## 动图和预览

- 14 个原创动作：200×120、24 帧、无限循环、每周期 2 秒，80/80/90 毫秒间隔近似 12 fps。
- GIF 合计 387,330 字节，嵌入静态示意 168,000 字节，总计 555,330 字节，低于 1 MiB 预算。
- 校验帧数、尺寸、周期、循环边界、运动差异、首帧 PNG 与清单 SHA256。
- 人工查看 `fitness-motion-review.png` 的每个动作四个关键姿态，确认器械轨迹与循环；资源是识别用简图，不是个人训练指导。
- 已生成并请求在 Codex 打开桌面、训练、休息和历史圆屏预览；另有要点、总结、设置、目标、模板、未校时和两类失败页面。
- 预览输出：`.cache/ui-preview/`；预览日期与记录仅为固定测试数据。

## 固件构建和交付

使用 Docker `espressif/idf:v5.5.3`，执行 `idf.py reconfigure build` 和 `idf.py merge-bin -o waveshare-launcher-usb.bin`，目标 ESP32-S3。

- `CONFIG_FITNESS_APP_GIF=1`、`CONFIG_LV_USE_GIF=1`；应用 Kconfig 自动选择 GIF 解码器。
- 沿用 16 MiB Flash、8 MiB 应用分区、24 KiB NVS，不修改分区表或增加联网服务。
- 最终应用 6,193,312 字节，合并镜像 6,258,848 字节；应用分区剩余 2,195,296 字节（约 26%）。构建源码的 572 个固件文件与独立 worktree 一致，仅 `sdkconfig.defaults` 的换行符格式不同。
- `dist/fitness/waveshare-launcher-usb.bin`：合并镜像，后续 USB 烧录使用。
- `dist/fitness/waveshare_launcher.bin`、`bootloader.bin`、`partition-table.bin`：分区文件。
- `dist/fitness/sha256.json`：四个固件文件的 SHA256。
- `dist/fitness/build-manifest.json`：实际文件大小、构建配置、源码基线和源码哈希清单。
- 已通过合并偏移 `0x0/0x8000/0x10000`、NVS 空白区 `0x9000–0xEFFF`、固件版本、SHA256 和 8 MiB 分区容量校验。

校验已交付文件：

```powershell
python scripts/verify_fitness_assets.py
python scripts/verify_artifacts.py --dist dist/fitness --build ../fitness-build/firmware/build
```

构建日志：`logs/fitness-build.log`。宿主日志：`logs/fitness-model.log`、`logs/fitness-preview.log`、`logs/fitness-preview-build.log`、`logs/codex-render.log`。
重新运行宿主测试和常规构建的命令见 `FITNESS.md`；主工作区同时存在其他改动时，务必先确认源码范围。

## 未完成的实机验收

本次没有 USB 烧录，也没有宣称实机验收完成。以下必须在原板完成：

1. 运行至少 30 分钟，连续切换动作和应用，检查内部 RAM/PSRAM、解码器与任务占用是否持续增长。
2. 检查动图流畅度、颜色、触摸响应、快速连点和系统上滑手势。
3. 在记组、休息、后台、修改设置及总结保存后断电，检查真实 NVS 恢复和未校时显示。
4. 检查真实 NVS 满或写入失败时的提示与重试，以及填满 30 条历史后的轮换。

宿主测试的断电/存储故障是可控模拟，不能替代物理断电和真实 NVS 故障测试。
