# 已知限制 (Known Limitations)

> 本文档记录当前 1.0 release candidate 阶段已知的功能缺失、设计边界与跨平台验证缺口。Phase 1（M0~M3）、Phase 2（M4）与 Phase 3（M5~M6）主体功能均已完成；Phase 4（M7~M8）仍用于发布工程、跨平台运行时验证、性能与稳定性回归。
> 术语口径以 [TERMINOLOGY.md](TERMINOLOGY.md) 为准。
> 这些不是 bug，而是尚未规划或排期到后续阶段的功能；其中部分项被显式标注为 **设计决策不做**，避免被重复提议。

---

## 布局与显示（Phase 2.5 范围 / 部分明确不做）

- **Layout Preset 管理 UI**：数据结构（`LayoutPreset`）已预留并持久化，但完整的创建 / 切换 / 删除 / 应用 UI 不属于 Phase 2 主体，需在 Phase 3/4 规划时单独排期。
- **Label 位置**：当前 `LabelPosition` 仅 `Top / Bottom`，足以满足 PRD 三种显示模式（`None / Overlay / Below`）。Left / Right / Center / 角落位置 **明确不做**（Phase 2.5 产品决策）。
- **Background fit 模式**：当前 `ImageFitMode` 仅 `Fit / Stretch`。Fill / Tile 等更多模式 **暂不扩展**，除非未来用户反馈明确。
- **Overlay 图层栈 / preset 管理**：当前只支持单层自定义 overlay 图片。多 overlay 图层栈、overlay preset 管理 UI、复杂混合模式 **明确不做**；未来内置 safe-zone 等图示通过插件 DLL 旁资源文件夹动态读取，不引入素材管理系统。
- **Safe Area preset**：当前仅 `EBU_R95`。SMPTE RP 218、Action Safe / Title Safe 等更多 preset **暂不扩展**；未来更多安全区图示优先作为 overlay 资源提供。
- **图像资源管理 / 素材库**：**明确不做**。插件核心定位是 multiview，不是素材管理；用户手动选择路径、Background/Overlay 路径直接持久化绝对路径。

## VU Meter

- ~~**Peak Hold**：尚未实现。~~ **已完成**（Phase 2.5）：`peakHoldEnabled`、`peakHoldMs`、`peakHoldDecayDbPerSec`、`peakHoldWidthPx`；渲染 hold marker + 硬化。
- ~~**dB 标尺 / 刻度**：尚未实现。~~ **已完成**（Phase 2.5）：`scaleEnabled`、`scaleTicks` CSV、`scaleShowLabels`（text source cache）、`scaleColor`、`scaleSide`；tick 全宽 + labels below tick + 全 alpha 可见。
- ~~**Per-cell trackMode**：需 Phase 2.5 单独决策。~~ **已决策**（Phase 2.5）：不做 per-cell trackMode override；Cell scope 下 Track Source / Manual Track 控件已禁用；字段保留以便未来无破坏扩展。
- **完整 OBS Mixer 复刻**：**明确不做**。VU meter 定位是监看辅助而非混音器。
- ~~**5.1 / 7.1 多声道独立 meter**：**低优先级**，当前不做。~~ **已完成**（issue #7）：VU meter 可按源实际声道数显示 multi-channel bars，宽度仍表示总厚度。第一版不显示每声道标签，也不提供手动声道布局 preset。
- **外部信号（NDI / Spout / 媒体流）音频 meter**：~~属于 Phase 3（M6）外部流接入范围。~~ **已完成**（M6.0 + M6.6）：FFmpeg / NDI / VLC 外部 cell 默认 metering 自己的私有源，与 trackMode 正交（内部 cell 仍走 streaming track）；Spout 无音频仍是 silence。

## 外部信号（Phase 3 / M6）

- **NDI / Spout 直连**：~~尚未实现~~ **已完成**（M6.2 / M6.3）：通过动态调用宿主 DistroAV (`ndi_source`) / obs-spout2 (`spout_capture`) 完成；不链接 SDK。
- **RTMP / HLS / FLV / SRT 拉流**：~~尚未实现~~ **已完成**（M6.1）：复用 OBS 内置 `ffmpeg_source` private source。VLC 二级 provider 见 M6.4。
- **VLC 二级 media provider**：**已完成**（M6.4）：复用 OBS 内置 `vlc_source` private source；当 OBS 构建时未带 libVLC 则 tab 自动 gate。
- **WebRTC 接入**：保留 enum + SourcePicker placeholder，runtime 不在 M6 实现。开放设计问题：
  - **传输与信令**：WHIP / WHEP / 自研协议 / 复用 obs-webrtc（如果未来 OBS Studio 内置）；信令通道（HTTP、WebSocket、SIP）。
  - **认证与 ICE 配置**：bearer token / OAuth / 自定义 header；STUN / TURN 服务器配置入口。
  - **编解码偏好**：H.264 / VP8 / VP9 / AV1；硬件解码是否需要与 OBS 主输出共享 GPU 资源。
  - **线程模型**：接收循环线程归属，与现有 supervisor 1Hz health-check 的协调。
  - **延迟与备播策略**：是否复用 NDI/Spout 的 discovery-driven SIGNAL LOST 语义，或需要 WebRTC 专属 reconnect timing。
  - **音频路径**：是否复用 rebuild_volmeters 既有外部 cell 通路；多通道 / opus 转译策略。
- ~~**Signal Lost / 断流策略**：重试、备播、彩条等断开行为尚未实现（属 Phase 3 / M5）。~~ **已全部完成**（[phase-3-acceptance-checklist.md](phase-3-acceptance-checklist.md)）：M5 内部源 Black / PlaceholderImage / ClearCell + Fallback (PGM/PRVW/Scene/Source/Image) + Reconnect Now + 动态生效；M6 外部源（NDI/Spout/FFmpeg/VLC）sticky display state 下的 SignalLost / RetryWithFallback / Reconnecting / Paused / FallbackActive overlay 均已接通。

## Signal Lost / Phase 3 / M5 范围内的边界

- **Duplicate Scene / Duplicate Sources（OBS Studio Mode）**：当用户在 OBS 顶部菜单 *Edit → Duplicate Scene* / *Duplicate Sources* 启用副本策略时，Preview 中删除的 source 在 Program 仍持有副本时**不会触发 `source_remove`**。Multiview cell 此时状态保持 Active（直到 Program 释放最后一个引用），符合 OBS source identity 语义；不属于本插件 bug。要求"PGM tree 不可达即 missing"属于 M7+ 的"逻辑可见性"特性，**M5 不做**。
- **fallback chain（多级链）**：第一版只支持单一 fallback；fallback 自身失败时直接退到 MISSING / SIGNAL LOST，不级联。**明确不做**。
- **placeholder / fallback static image FillCrop fit 模式**：当前仅 `Stretch / Fit`，与 bg-image 一致。FillCrop **暂不扩展**。
- **status overlay 文案多语言**：`MISSING SOURCE / MISSING SCENE / FALLBACK` 当前为英文常量，未走 OBS locale。Phase 4 再考虑。
- **placeholder / fallback 图片纹理跨 instance / 跨 window 不共享**：每个 MultiviewWindow 独立 4 阶段加载，同一图片被多实例引用时会重复加载。属性能优化项，当前用例数量级小（≤数张图），**不做**。

## 实例管理

- **实例排序（Move Up / Move Down / 拖放）**：按钮已预留但禁用。后续计划支持鼠标拖放排序 + 按钮辅助排序，排序结果持久化。
- ~~**删除已打开实例**：当前删除实例不会自动关闭已打开的对应窗口（窗口可能变为孤儿状态）。~~ **已修复**：[src/manager-dialog.cpp](../src/manager-dialog.cpp) `on_delete_instance()` 在删除前调用 `close_multiview_window(uuid)` 关闭对应窗口。

## Source Identity

- **基于 name 的引用**：当前 Scene / Source assignment 以名称为主键。删除后 undo 恢复已通过 lazy re-resolve 机制支持（按名重新查找）。基于 UUID 的完整匹配排期到后续 Milestone。

## Cell Assignment 保存语义

- **自动保存**：当前 cell assignment 修改（添加 / 更换 / 清空）会立即自动保存。后续如需 dirty workflow 需重新设计。

## Visual Settings 保存语义

- **整对象保存**：当前 `CellDisplaySettingsDialog` accept 时无条件 save，不利用 `dirty_` 标志做最小写入。10x10 grid 下改一个字段会触发整 instance JSON 重写。属 [phase-2-hardening-notes.md](phase-2-hardening-notes.md) 观察项，Phase 2.5 不强制优化。

## 分发

- **Release artifacts**：GitHub Actions tag workflow 已能产出 release artifact，包括 Windows user-layout zip、Windows portable-root zip、Ubuntu artifact、source tarball，以及按 CI 配置生成的其它平台 artifact。正式发布仍应人工检查 artifact 内容和 draft release 说明。
- **Installer**：Windows 安装器仍未作为主要发布形态提供；当前推荐 portable-root zip 或 OBS 用户插件布局 zip。
- **Prerelease 版本号**：`1.0.0-rc.1` / `1.0.0-beta.1` 等版本号可写入 `buildspec.json` 并用于 tag。CI 会临时把版本规范化为 `1.0.0` 供 CMake configure 使用，然后恢复完整 prerelease 版本用于 artifact / release 命名。详见 [DEVELOPMENT.md](DEVELOPMENT.md#发布流程)。

## 平台与版本验证

- **macOS / Linux 运行时**：CI 构建配置存在，但未经充分运行时测试验证。
- **OBS 32.0**：尚未在 OBS 32.0 上完整验证；已确认 31.1.1 与 32.1.x Windows portable 单机可用。
- **tag 构建**：`1.0.0-rc.1` / `1.0.0-rc.2` tag workflow 已跑通过并暴露过 artifact 命名 / portable zip 布局问题，相关 CI 修复已落地。后续每个 release tag 仍需人工检查 draft release 与下载包结构。

---

## 历史已移除 / 已否决的功能

以下功能曾被讨论或预留过，但已明确否决，避免在后续提案中重复出现：

- ~~**Folder grouping（实例文件夹分组）**：~~ Phase 1（M0~M3）阶段中途移除，理由是简化实例管理。空 folder 持久化问题已不存在。
- ~~**右键菜单中的克隆 / 删除整个 Multiview 实例**：~~ 这些操作仅在管理 / 设置 Dialog 中提供，避免误操作（详见 [ROADMAP.md](ROADMAP.md) §1.9）。
- ~~**复制网格 UUID 入口（用户向）**：~~ Phase 1 不需要；未来如需要，可放到高级 / debug 区域。
- ~~**字段级 Visual Settings 继承**：~~ Phase 2 决策为分组级继承（Background / Label / SafeArea / VuMeter / Overlay 各自独立 `InheritanceMode`），避免 UI 碎片化。
- ~~**Per-cell Highlight override**：~~ Highlight 由 cell 与 PGM/PRVW scene tree 的关系驱动，per-cell override 无语义。Highlight scope 限定在 Global / Instance。
