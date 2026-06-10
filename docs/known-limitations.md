# 已知限制 (Known Limitations)

> 本文档记录当前版本（0.2.x，Phase 2 / M4 主体完成，Phase 2.5 收尾中）已知的功能缺失、设计边界与跨平台验证缺口。
> 术语口径以 [docs/TERMINOLOGY.md](docs/TERMINOLOGY.md) 为准。
> 这些不是 bug，而是尚未规划或排期到后续阶段的功能；其中部分项被显式标注为 **设计决策不做**，避免被重复提议。

---

## 布局与显示（Phase 2.5 范围 / 部分明确不做）

- **Layout Preset 管理 UI**：数据结构（`LayoutPreset`）已预留并持久化，但完整的创建 / 切换 / 删除 / 应用 UI 不属于 Phase 2 主体，需在 Phase 2.5 之后单独规划。
- **Label 位置**：当前 `LabelPosition` 仅 `Top / Bottom`，足以满足 PRD 三种显示模式（`None / Overlay / Below`）。Left / Right / Center / 角落位置 **明确不做**（Phase 2.5 产品决策）。
- **Background fit 模式**：当前 `ImageFitMode` 仅 `Fit / Stretch`。Fill / Tile 等更多模式 **暂不扩展**，除非未来用户反馈明确。
- **Overlay 图层栈 / preset 管理**：当前只支持单层自定义 overlay 图片。多 overlay 图层栈、overlay preset 管理 UI、复杂混合模式 **明确不做**；未来内置 safe-zone 等图示通过插件 DLL 旁资源文件夹动态读取，不引入素材管理系统。
- **Safe Area preset**：当前仅 `EBU_R95`。SMPTE RP 218、Action Safe / Title Safe 等更多 preset **暂不扩展**；未来更多安全区图示优先作为 overlay 资源提供。
- **图像资源管理 / 素材库**：**明确不做**。插件核心定位是 multiview，不是素材管理；用户手动选择路径、Background/Overlay 路径直接持久化绝对路径。

## VU Meter（Phase 2.5 polish 设计待定）

- **Peak Hold**：尚未实现。Phase 2.5 计划补充 `peakHoldMs` 字段并渲染保持线。
- **dB 标尺 / 刻度**：尚未实现。Phase 2.5 计划默认渲染 `-60 / -40 / -20 / -9 / 0` 几个刻度。
- **Per-cell trackMode**：当前 `VuMeterSettings.trackMode` 按 instance/window-wide 计算（[src/multiview-window.cpp](src/multiview-window.cpp) `compute_active_track_bit()` 中明确注释 deferred）；scene/source cell 的 trackMode 语义需 Phase 2.5 单独决策。
- **完整 OBS Mixer 复刻**：**明确不做**。VU meter 定位是监看辅助而非混音器。
- **5.1 / 7.1 多声道独立 meter**：**低优先级**，Phase 2.5 不做。
- **外部信号（NDI / Spout / 媒体流）音频 meter**：属于 Phase 3（M6）外部流接入范围。

## 外部信号（Phase 3 / M6）

- **NDI / Spout 直连**：尚未实现，需后续阶段通过动态调用宿主 obs-ndi / obs-spout2 插件完成。
- **RTMP / HLS / FLV / SRT 拉流**：尚未实现。
- **WebRTC 接入**：尚未实现。
- **Signal Lost / 断流策略**：重试、备播、彩条等断开行为尚未实现（属 Phase 3 / M5）。

## 实例管理

- **实例排序（Move Up / Move Down / 拖放）**：按钮已预留但禁用。后续计划支持鼠标拖放排序 + 按钮辅助排序，排序结果持久化。
- ~~**删除已打开实例**：当前删除实例不会自动关闭已打开的对应窗口（窗口可能变为孤儿状态）。~~ **已修复**：[src/manager-dialog.cpp](src/manager-dialog.cpp) `on_delete_instance()` 在删除前调用 `close_multiview_window(uuid)` 关闭对应窗口。

## Source Identity

- **基于 name 的引用**：当前 Scene / Source assignment 以名称为主键。删除后 undo 恢复已通过 lazy re-resolve 机制支持（按名重新查找）。基于 UUID 的完整匹配排期到后续 Milestone。

## Cell Assignment 保存语义

- **自动保存**：当前 cell assignment 修改（添加 / 更换 / 清空）会立即自动保存。后续如需 dirty workflow 需重新设计。

## Visual Settings 保存语义

- **整对象保存**：当前 `CellDisplaySettingsDialog` accept 时无条件 save，不利用 `dirty_` 标志做最小写入。10x10 grid 下改一个字段会触发整 instance JSON 重写。属 [docs/phase-2-hardening-notes.md](docs/phase-2-hardening-notes.md) 观察项，Phase 2.5 不强制优化。

## 分发

- **Installer / Portable 正式 Artifacts**：尚未提供正式安装器或便携版打包脚本，当前仅支持手动复制 DLL。属 Phase 4 / M7。

## 平台与版本验证

- **macOS / Linux 运行时**：CI 构建配置存在，但未经充分运行时测试验证。
- **OBS 32.0**：尚未在 OBS 32.0 上完整验证 Phase 2 功能（已确认 31.1.1 与 32.1 单机可用）。
- **tag 构建**：尚未通过 GitHub tag 触发完整 Release artifact 构建并验证。

---

## 历史已移除 / 已否决的功能

以下功能曾被讨论或预留过，但已明确否决，避免在后续提案中重复出现：

- ~~**Folder grouping（实例文件夹分组）**：~~ Phase 1（M0~M3）阶段中途移除，理由是简化实例管理。空 folder 持久化问题已不存在。
- ~~**右键菜单中的克隆 / 删除整个 Multiview 实例**：~~ 这些操作仅在管理 / 设置 Dialog 中提供，避免误操作（详见 [plan.md](plan.md) §1.9）。
- ~~**复制网格 UUID 入口（用户向）**：~~ Phase 1 不需要；未来如需要，可放到高级 / debug 区域。
- ~~**字段级 Visual Settings 继承**：~~ Phase 2 决策为分组级继承（Background / Label / SafeArea / VuMeter / Overlay 各自独立 `InheritanceMode`），避免 UI 碎片化。
- ~~**Per-cell Highlight override**：~~ Highlight 由 cell 与 PGM/PRVW scene tree 的关系驱动，per-cell override 无语义。Highlight scope 限定在 Global / Instance。

