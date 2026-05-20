# OBS Advanced Multiview 开发计划

> 本文件是给项目成员与后续 Copilot session 共同阅读的主开发计划。  
> 当前版本已从最初的 obs-plugintemplate 骨架进入 Phase 2 设计阶段。  
> 更细的阶段设计、验收、限制与开发工作流见 `docs/` 下的专题文档。

---

## 0. 当前项目状态

当前主线状态：

```text
当前基线版本：0.1.2
当前阶段：Phase 2 设计完成，准备进入 Phase 2 实现
Phase 1：已完成
Phase 1.5：已完成
Phase 2：Visual Settings / Cell Presentation
```

当前已完成：

- Phase 1 / Milestone 0 ~ 3：基础 MVP 闭环。
- Phase 1.5：验收清单、已知限制、hardening notes、文档卫生与 baseline 整理。
- Phase 2 设计文档：`docs/phase-2-visual-settings-design.md`。

当前主线已经具备：

- OBS 工具菜单入口；
- 管理 / 设置 Dialog；
- Multiview 实例创建、重命名、克隆、删除；
- 打开 / 聚焦实例窗口；
- 配置系统与 `plugin_config` 存储；
- 基础网格与 span 布局；
- coordinate-based cell assignments；
- span absorption merge；
- MultiviewWindow；
- Source Picker；
- PGM / PRVW / Scene / Source；
- OBS 内部源 lazy re-resolve；
- source 删除 / undo 后按名称重新解析的基础支持；
- 全屏；
- 置顶；
- OBS 风格 toolbar icon；
- 0.1.2 baseline CI 成功记录。

---

## 1. 相关参考文档

后续 Copilot session 必须优先阅读以下文档：

```text
plan.md
README.md
docs/phase-2-visual-settings-design.md
docs/phase-1-acceptance-checklist.md
docs/phase-1-hardening-notes.md
docs/known-limitations.md
docs/phase-1-development-breakdown.md
docs/ui-ascii-wireframes.md
docs/DEVELOPMENT.md
docs/setup/README.md
```

其中：

- `plan.md`：项目主路线与阶段定义。
- `docs/phase-2-visual-settings-design.md`：Phase 2 的详细设计，是 Phase 2 开发的主要约束文档。
- `docs/phase-1-acceptance-checklist.md`：Phase 1 baseline 验收依据。
- `docs/phase-1-hardening-notes.md`：Phase 1 hardening 记录。
- `docs/known-limitations.md`：当前已知限制与非 bug 缺失项。
- `docs/ui-ascii-wireframes.md`：UI/UX 参考。
- `docs/DEVELOPMENT.md`：构建、部署、测试、提交前检查流程。

---

## 2. 项目目标与范围

OBS Advanced Multiview 是一个面向 OBS Studio 的增强型 multiview 插件，目标是替代并扩展 OBS 内置 Multiview 能力。

核心定位：

- 高度自定义的多画面监看工具。
- 基于 Qt 与 OBS 核心架构。
- 支持自定义网格数量与 span 合并。
- 支持多个独立 Multiview 实例。
- 支持窗口化与全屏显示。
- 第一阶段支持 OBS 内部信号：PGM、PRVW、Scene、Source。
- 后续阶段再考虑外部信号：NDI、Spout、RTMP、HLS、FLV、SRT、WebRTC 等。
- 尽量复用 OBS 内置 Qt / frontend 能力，减少额外 artifacts 体积。
- 运行稳定性、鲁棒性、边界处理和优雅错误处理优先。
- 插件内部异常不得影响 OBS 主线程、主输出或其他插件。

最低 OBS 版本目标：

- 最低支持 OBS 31.1.1。
- 同时确保兼容 OBS 32.0 和 32.1。
- 当前开发环境与部署脚本重点覆盖 OBS 31.1.1 与 32.1.x。

---

## 3. 已确认的关键产品决策

### 3.1 管理入口

- 不做 dock 作为第一阶段入口。
- 在 OBS 顶部菜单的“工具”下拉菜单内提供管理 dialog 入口。
- 点击后打开 OBS Advanced Multiview 的管理 / 设置页面。
- 通过管理 / 设置页面打开具体 Multiview 实例。

### 3.2 Multiview 实例与窗口

已确认：

- 一个 Multiview 实例同一时间只允许一个可编辑窗口。
- 如果用户重复打开同一个实例，则聚焦已有窗口，而不是创建第二个同 UUID 窗口。
- 如果用户需要多个相同或相似的窗口，应在管理页面中克隆整个 Multiview 实例。
- 克隆后生成新的实例 UUID，并可独立编辑、保存和打开。
- 删除整个 Multiview 实例应在管理 / 设置页面中完成。
- 关闭窗口不等于删除实例配置。

暂不做：

- 同一个 Multiview 实例打开多个不同步的可编辑窗口。
- 同一个 Multiview 实例多窗口自动同步编辑。
- 多窗口冲突合并。

### 3.3 布局能力

已确认：

- 使用基础网格 + span 的布局模型。
- 用户最多可以选择 10x10 网格。
- 用户可以将多个小格合并成较大的 span。
- 不支持像素级微调。
- 不支持任意自由矩形布局。
- 不支持拖拽到任意像素尺寸的 resize。
- 每个信号画面保持源比例显示。
- 非等比区域使用 letterbox / pillarbox，不做铺满拉伸或裁切。

布局限制：

- rows 范围：1 到 10。
- columns 范围：1 到 10。
- span 不得小于 1x1。
- span 不得超出基础网格边界。
- cell / region 不得重叠。
- gutter / border 厚度支持 0 到 50 px。
- gutter / border 是 cell 上下左右之间的间距 / 边缘，不仅是左右缝隙。

当前实现状态：

- 已实现基础网格与 span。
- 已实现 coordinate-based cell assignments，避免 grid resize / span 变化导致 unrelated assignment 位移。
- 已实现 span absorption merge：合并时允许吸收完全包含在新合并区域内的已有 span。
- 已实现 Reset All。
- 已实现 zone separators。

### 3.4 OBS 内部信号范围

Phase 1 已支持：

- PGM。
- PRVW。
- 任意 Scene。
- 任意 Source。

当前仍不属于 Phase 2：

- NDI。
- Spout。
- RTMP。
- HLS / M3U8。
- FLV。
- SRT。
- WebRTC。

### 3.5 Source Picker

Phase 1 已采用：

- 列表 + 搜索。

暂不做：

- OBS 32 风格卡片预览 source picker。
- 实时缩略图网格选择器。

后续可参考 OBS 32 新版 Add Source 交互，但不作为 Phase 2 要求。

### 3.6 保存语义

当前实际行为：

- cell assignment 修改后自动保存。
- 布局修改后自动保存。
- Phase 2 的视觉参数修改也应自动保存。
- 当前不再依赖右键菜单中的“Save Cell Assignments”作为更新前置条件。

历史规划中提到的“保存布局 / 保存网格信号分离”仍可作为语义参考，但当前产品交互已向 auto-save 收敛。

Phase 2 需要遵守：

```text
修改视觉参数 -> 自动保存 -> 已打开 MultiviewWindow 动态刷新
```

### 3.7 Layout Preset

Layout Preset 采用独立建模。

Layout Preset 的语义：

- Layout Preset 是独立于 Multiview 实例的布局预设。
- Layout Preset 只保存布局相关内容。
- Multiview 实例可以选择、复制或基于某个 Layout Preset 创建自己的布局。
- Layout Preset 不保存具体 cell 绑定的 PGM / PRVW / Scene / Source。
- Layout Preset 不保存网格信号。

Layout Preset 保存内容：

- 名称。
- UUID。
- rows。
- columns。
- span 合并结构。
- gutter / border 厚度。
- 与布局相关的默认显示参数。

当前状态：

- 数据结构已预留。
- 完整 Layout Preset 管理 UI 不属于 Phase 2 目标。

### 3.8 配置存储

已确认：

- 配置直接存储在 OBS 的 `plugin_config` 路径下。
- 不自行区分安装版和便携版路径。
- 安装版与便携版路径差异交由 OBS 配置目录机制处理。
- 参考 Advanced Scene Switcher 的配置存储方式。

建议目录：

```text
plugin_config/obs-advanced-multiview/
```

配置文件按当前 OBS 场景集合隔离。

建议文件名：

```text
settings-<场景集合名>.json
```

配置写入要求：

- 必须避免写入半截 JSON 导致配置损坏。
- 保存应使用安全写入机制。
- JSON 内必须包含 `configVersion`，以支持后续迁移。

当前状态：

- 已有 config path hardening。
- 已避免 config path 为空时写入未知工作目录。
- Phase 2 需要继续保持旧配置兼容。

### 3.9 右键菜单

Phase 1 当前右键菜单已根据实际产品行为调整。

MultiviewWindow 右键菜单原则：

- 根据命中位置显示 cell 级操作或 window 级操作。
- 不在 MultiviewWindow 右键菜单中放实例级管理操作。
- 不放“打开 / 聚焦窗口”。
- 不放“克隆”。
- 不放“删除整个 Multiview 实例”。
- 不放“复制网格 UUID”。

Phase 2 可新增：

```text
Cell Display Settings...
```

但必须注意：

- 右键菜单可以作为快捷入口。
- 视觉参数不能只藏在右键菜单中。
- Global / Instance / Cell 三层设置都必须在管理 / 设置 Dialog 中有清晰入口。

### 3.10 动态生效

PRD 要求修改网格配置、增删信号源、更改断流策略等操作实时生效，无需重启 OBS。

当前确认原则：

- 布局变化应对已打开的当前 MultiviewWindow 动态生效。
- 添加 / 更换 / 清空 cell 后，当前 MultiviewWindow 应动态更新。
- Phase 2 中，视觉参数修改也必须动态生效。
- 后续断流策略修改也应动态生效。

### 3.11 Qt 窗口句柄与独立渲染管线

PRD 中提到结合 Qt 窗口句柄构建独立硬件加速渲染管线。

当前状态：

- Phase 1 已形成基于 MultiviewWindow 的独立显示与渲染基础。
- Windows Always-on-Top 已改为使用 `SetWindowPos`，避免 `setWindowFlags()` 引发 native HWND 重建导致闪烁与渲染目标失效。
- Phase 2 不应大幅重构窗口句柄生命周期，除非为视觉参数渲染所必需。
- 相关性能与稳定性仍需在后续回归阶段持续验证。

### 3.12 外部流重连策略

外部流不属于 Phase 2，但已确认未来要求：

- 不能使用 while true 空转重试。
- 重试应使用 backoff。
- 必须提供用户手动强制重连能力。
- 不能让用户在信号已经恢复时只能等待例如 30 秒的 backoff。

未来可考虑的入口：

- cell 右键菜单中的“立即重连”。
- 断流 overlay 上的“立即重连”按钮。
- 信号源编辑 dialog 中的“立即重连”按钮。

不可见 / 最小化窗口策略：

- 对 OBS 内部信号，窗口不可见或最小化时可以降帧或暂停绘制。
- 对 NDI / Spout / 网络拉流，窗口不可见不应简单等同于停止拉流。
- 外部流后续应区分 render、transport、decode、buffer、health check。
- 每类 SignalProvider 自行决定不可见时策略。

---

## 4. 建议的软件分层

### 4.1 MultiviewManager

职责：

- 插件级管理器。
- 管理所有 Multiview 实例。
- 管理全局设置。
- 管理配置加载与保存。
- 管理实例打开、聚焦、关闭、克隆、删除。
- 管理 Layout Preset 数据结构。
- 在 OBS 退出时统一关闭窗口并释放资源。
- 负责工具菜单入口打开的管理 / 设置 dialog。

不负责：

- 每帧渲染。
- 具体 OBS source 绘制。
- 外部流传输。

### 4.2 MultiviewInstance / MultiviewModel

职责：

- 表示一个 Multiview 实例的数据模型。
- 保存实例 UUID。
- 保存实例名称。
- 保存 rows / columns。
- 保存 span 布局。
- 保存 coordinate-based cell assignments。
- 保存 instance level visual settings。
- 保存 dirty / auto-save 所需状态。
- 可记录其布局是否来源于某个 Layout Preset。

### 4.3 Layout Preset

职责：

- 表示独立的布局预设。
- 保存 Layout Preset UUID。
- 保存 Layout Preset 名称。
- 保存 rows / columns。
- 保存 span 合并结构。
- 保存 gutter / border 厚度。
- 保存与布局相关的默认显示参数。

不负责：

- 保存 PGM / PRVW / Scene / Source 绑定。
- 保存 cell assignments。
- 保存外部流配置。

### 4.4 MultiviewWindow

职责：

- 某个 Multiview 实例的独立 Qt 窗口。
- 窗口化显示。
- 全屏切换。
- 窗口置顶。
- 右键菜单。
- 鼠标 hit-test。
- 判断右键命中已有信号 cell、空白 cell 或 gutter。
- 打开“添加 / 选择信号源”或“更换 / 编辑信号源”对话框。
- 打开 Phase 2 的 Cell Display Settings 入口。
- 触发清空 cell。
- 打开当前 Multiview 的编辑网格页面。
- 打开全局设置页面。
- 关闭当前窗口。

不建议负责：

- NDI / Spout / 网络拉流传输逻辑。
- 外部流重连状态机。
- 每种信号 provider 的内部生命周期。

### 4.5 SignalProvider / SignalRuntime

职责：

- 抽象每个 cell 的信号来源。
- Phase 1 支持 OBS 内部信号。
- 未来支持外部信号。
- 提供信号状态、显示名称、错误状态等。
- 后续负责外部流连接、断开、重试、fallback 状态。

当前 provider：

- Program provider。
- Preview provider。
- Scene provider。
- Source provider。

未来 provider：

- NDI provider。
- Spout provider。
- Media / VLC provider。
- SRT / RTMP / HLS / FLV provider。
- WebRTC provider。

### 4.6 MultiviewRenderer

职责：

- 实际渲染 Multiview 画面。
- 绘制 cell 背景。
- 绘制 OBS source / scene / PGM / PRVW。
- 绘制黑场。
- 绘制标签。
- 绘制边框 / gutter。
- 绘制选中态 / hover 态。
- Phase 2 绘制 background、label、overlay、safe area、VU meter。
- 后续绘制 lost signal 占位图、fallback 等。
- 保持画面比例，使用 letterbox / pillarbox。

不负责：

- 保存配置。
- 打开右键菜单。
- 处理外部流连接。

### 4.7 Visual Settings Resolver

Phase 2 建议新增逻辑层，概念上负责：

- 解析 Global / Instance / Cell 三层视觉设置。
- 计算 `EffectiveCellVisualSettings`。
- 处理分组级继承。
- 为 Renderer 提供只读的最终视觉配置。

该逻辑可以先作为函数或静态 helper 实现，不一定立刻拆独立类，但语义必须清楚。

---

## 5. Phase 1：基础可用闭环（已完成）

Phase 1 由 Milestone 0 到 Milestone 3 组成，当前已完成基础实现。

### 5.1 Milestone 0：仓库整理与基础骨架（已完成）

已完成目标：

- 插件可被 OBS 加载。
- OBS 工具菜单中出现 OBS Advanced Multiview 管理入口。
- 点击入口可打开基础管理 dialog。
- 插件能定位到自身 `plugin_config/obs-advanced-multiview/` 目录。
- OBS 退出时插件能正常卸载。

### 5.2 Milestone 1：配置系统与管理 / 设置 Dialog（已完成）

已完成目标：

- JSON 配置结构。
- `configVersion`。
- 配置按当前 OBS 场景集合分文件保存。
- Multiview 实例列表。
- 新建实例。
- 重命名实例。
- 克隆实例。
- 删除实例。
- 打开 / 聚焦实例窗口。
- Layout Preset 数据结构预留。
- 配置保存失败时不破坏旧配置。

当前实现补充：

- 使用 QTabWidget。
- 使用 OBS 风格 icon button。
- folder 功能曾被尝试后移除，不属于当前 baseline。
- 实例排序仍为后续增强。

### 5.3 Milestone 2：布局引擎与编辑网格（已完成）

已完成目标：

- LayoutEngine。
- rows / columns 1 到 10。
- span。
- span 越界 / 重叠处理。
- gutter / border 0 到 50 px。
- cell / region rect 计算。
- video rect 计算。
- hit-test。
- 编辑网格页面。
- 布局动态应用到已打开窗口。

当前实现补充：

- coordinate-based cell assignments。
- span absorption merge。
- Reset All。
- zone separators。

### 5.4 Milestone 3：MultiviewWindow 与 OBS 内部信号渲染（已完成）

已完成目标：

- MultiviewWindow。
- 基础 renderer。
- 窗口化显示。
- 全屏。
- 窗口置顶。
- 关闭窗口。
- 右键菜单。
- Source Picker：列表 + 搜索。
- PGM。
- PRVW。
- Scene。
- Source。
- 清空 cell。
- cell assignment 自动保存。
- 动态刷新。
- OBS source 删除或失效时不崩溃。
- OBS 退出时关闭所有 MultiviewWindow 并释放引用。

当前实现补充：

- lazy re-resolve dead source refs。
- 可配置 re-resolve rate。
- PRVW fallback indicator。
- Windows Always-on-Top 使用 `SetWindowPos`，避免 HWND 重建闪烁。
- 删除实例时应避免留下孤儿窗口的策略已进入 hardening 范围。

---

## 6. Phase 1.5：验收、硬化与版本基线（已完成）

Phase 1.5 用于在进入 Phase 2 前固化 Phase 1 baseline。

已完成内容：

- `docs/phase-1-acceptance-checklist.md`。
- `docs/known-limitations.md`。
- `docs/phase-1-hardening-notes.md`。
- 文档中的旧 plugintemplate 名称修正。
- config path hardening。
- 未实现 / 不适合当前阶段的 UI 行为清理。
- 0.1.2 baseline。
- master / tag CI 成功记录。

Phase 1.5 完成后，进入 Phase 2。

---

## 7. Phase 2：视觉参数与 Cell Presentation（当前阶段）

Phase 2 的详细设计见：

```text
docs/phase-2-visual-settings-design.md
```

Phase 2 的核心目标：

- 建立 Global / Instance / Per Cell 三层视觉设置体系。
- 建立分组级继承模型。
- 建立 `EffectiveCellVisualSettings` 解析逻辑。
- 建立 `CellRect` / `SignalRect` 坐标空间。
- 实现 label / background / overlay / safe area / VU meter。
- 所有视觉参数 auto-save。
- 所有视觉参数动态生效。
- 保持 Phase 1 稳定性与旧配置兼容。

Phase 2 明确不包含：

- NDI。
- Spout。
- RTMP。
- HLS / M3U8。
- FLV。
- SRT。
- WebRTC。
- Signal Lost 完整策略。
- fallback 图片 / 视频。
- 外部流 backoff 重试。
- 外部流立即重连。
- 完整 Layout Preset 管理 UI。
- installer / portable 正式发布系统。

### 7.1 Milestone 2.0：Visual Settings 架构与继承模型

目标：

- 建立 global / instance / per cell 三层视觉设置。
- 建立 visual settings 数据模型。
- 建立 effective config 解析。
- 建立 JSON schema。
- 建立动态更新通路。

范围：

- `GlobalVisualSettings`。
- `InstanceVisualSettings`。
- `CellVisualSettings`。
- `EffectiveCellVisualSettings`。
- 分组级继承。
- auto-save。
- 旧配置兼容。

验收标准：

- 旧配置正常加载。
- 缺字段正常 fallback。
- instance 可继承 global。
- cell 可继承 instance。
- effective config 计算正确。
- 修改后当前 MultiviewWindow 动态刷新。
- 不破坏 Phase 1 行为。

### 7.2 Milestone 2.1：Label Settings / 标签系统

目标：

- 实现标签名显示模式。
- 实现字体、字号、缩放模式、文字颜色、背景透明度、背景圆角等。
- 完成 label 的 instance / cell 继承体系。

范围：

- `None / Overlay / Below`。
- `fontFamily`。
- `fontSize`。
- `fontScaleMode`。
- `minFontSize / maxFontSize`。
- `textColor`。
- `backgroundOpacity`。
- `backgroundRounded`。
- 基础位置设置。

验收标准：

- 三种模式正常。
- Fixed / ScaleWithCell 正常。
- 10x10、span cell 正常。
- 重启恢复。
- 动态生效。

### 7.3 Milestone 2.2：Background Settings / 底色与底图

目标：

- 实现底色。
- 实现底图基础能力。
- 支持透明源背景与空 cell 背景。

范围：

- `backgroundColor`。
- `backgroundImagePath`。
- `backgroundImageFitMode`。
- instance / cell 继承。

验收标准：

- 底色正常。
- 底图正常。
- 图片不存在不崩。
- 透明源表现正确。
- 动态生效。
- 重启恢复。

### 7.4 Milestone 2.3：Overlay 坐标空间与前景 overlay

目标：

- 建立 `CellRect / SignalRect` 坐标空间。
- 实现前景 overlay。
- 支持 overlay 锚点模式。

范围：

- `CellRect`。
- `SignalRect`。
- `OverlayAnchorMode`。
- 前景 overlay 的路径、透明度、fit mode。

验收标准：

- Signal anchored overlay 在 letterbox / pillarbox 下行为正确。
- Cell anchored overlay 在 cell 区域内行为正确。
- span cell 正常。
- 动态生效。

### 7.5 Milestone 2.4：Safe Area / 安全区

目标：

- 实现安全区开关。
- 第一版支持 EBU R95。
- 安全区基于 SignalRect 绘制。

范围：

- `safeAreaEnabled`。
- `safeAreaPreset`。
- `safeAreaColor`。
- `safeAreaOpacity`。

验收标准：

- 安全区只在实际画面区域内。
- 不画到黑边。
- 动态生效。
- 重启恢复。

### 7.6 Milestone 2.5：VU Meter v1

目标：

- 参考 OBS 内置 VU meter 样式与能力。
- 实现基础 cell VU meter。

范围：

- `vuMeterEnabled`。
- `vuMeterPosition`。
- `vuMeterOpacity`。
- `vuMeterStyle`。
- 最小可用音频 meter 通路。

验收标准：

- 有音频 source 正常显示。
- 无音频 source 安全隐藏或空表。
- 删除 source 不崩。
- 多 cell 情况下性能可接受。
- 动态生效。
- 重启恢复。

### 7.7 Milestone 2.6：Visual Settings UI 整合

目标：

- 将 global / instance / cell 视觉设置整合到 UI。
- 避免所有功能只藏在右键菜单中。

范围：

- Settings tab 中的 Global Visual Settings。
- instance detail 中的 Instance Visual Settings。
- Cell Display Settings Dialog。
- 右键快捷入口。

VU Meter 扩展设置（在 2.6 UI 中暴露）：

- **位置**：四边任意一边（Top / Bottom / Left / Right）。
- **显示区域**：可选 cell 内显示或 signal（源画面）内显示。
- **宽度**：VU meter 条的像素宽度。
- **长度比例**：VU meter 占满 cell/signal 对应边的比例（0.0~1.0），默认 1.0。
- **绿/黄/红分界 dB 值**：可自定义 warning（默认 -20dB）和 error（默认 -9dB）阈值。
- **衰减速率**：Fast / Medium / Slow 三档（对应 OBS 的 23.5 / 11.76 / 8.57 dB/s）。
- **Flip 反转**：翻转 VU meter 的方向。
  - 垂直模式：正常 = 底部0dB顶部-∞，Flip = 顶部0dB底部-∞。
  - 水平模式：正常 = 左侧-∞右侧0dB，Flip = 左侧0dB右侧-∞。
- **透明度**：0.0~1.0。

验收标准：

- Global / Instance / Cell 都有清晰入口。
- 继承关系清晰。
- 动态生效。
- auto-save。
- 不阻塞 OBS。
- VU Meter 所有扩展设置在 UI 中可配置并实时生效。

### 7.8 Milestone 2.7：Phase 2 回归与性能验收

目标：

- 对 Phase 2 全部视觉能力做统一回归。
- 确认 0.2.x baseline。

范围：

- 配置迁移。
- 旧配置兼容。
- 动态更新。
- 多窗口。
- 多显示器。
- OBS 退出。
- source 删除。
- 10x10。
- span。
- 高 DPI。
- CI。
- 文档更新。

验收标准：

- OBS 31.1.1 正常。
- OBS 32.1.x 正常。
- 1x1 / 4x4 / 5x5 / 10x10 正常。
- 多窗口正常。
- source 删除不崩。
- 动态生效稳定。
- 性能可接受。
- 文档同步完成。

---

## 8. Phase 3：断开 / 删除 / Signal Lost 行为（后续）

Phase 3 对应原 Milestone 5。

当前仅保留已讨论和 PRD 中已列明方向。

OBS 内部源删除时，未来需支持的方向：

- 显示占位图。
- 保持黑场。
- 清空并释放该网格。
- 如果用户 ctrl+z 恢复，尝试名称匹配或更稳定的身份匹配。

外部流断开时，未来需支持的方向：

- 仅重试。
- 重试 + 备播。
- Signal Lost 图。
- 黑场或重试状态。
- 重连成功后切回主画面。

已确认的技术原则：

- 不使用 while true 空转重试。
- 使用 backoff。
- 必须提供“立即重连”。
- 防止用户连续点击导致疯狂重连。
- 插件关闭时后台任务必须可取消。

需要等待 Phase 2 完成后再详细规划：

- 第一版 lost signal UI。
- 状态机字段。
- fallback 类型。
- 内部源恢复匹配策略。
- 是否按 cell 配置策略。
- 断流策略修改后如何对当前 MultiviewWindow 动态生效。

---

## 9. Phase 4：外部流接入（后续）

Phase 4 对应原 Milestone 6，且部分依赖 Phase 3。

未来目标方向：

- NDI。
- Spout。
- RTMP。
- HLS / M3U8。
- FLV。
- SRT。
- WebRTC。

已确认策略方向：

- NDI 与 Spout 不直接集成 SDK。
- NDI / Spout 应尽量动态调用宿主环境中已安装的第三方插件能力。
- 目标是避免 SDK 版本不一致导致 OBS 内和 Multiview 内行为不一致。
- RTMP / HLS / FLV / SRT 可考虑复用 OBS 内置媒体源或 VLC 源。
- WebRTC 通过定制扩展接入，需后续详细规划。
- 外部流不需要事先添加到 OBS 场景列表中。
- 外部流应作为 Multiview 层面的信号来源。

不可见 / 最小化时的原则：

- 不简单等同于停止拉流。
- 需要区分 render、transport、decode、buffer、health check。
- 每类 SignalProvider 自行决定不可见时行为。

需要等待 Phase 3 / Phase 4 详细规划：

- 如何检测 obs-ndi / DistroAV 插件。
- 如何检测 obs-spout2 插件。
- 如何创建 private source。
- 如何处理不同插件版本 settings schema。
- 外部流 source picker UI。
- 外部流状态机。
- 重试、立即重连、fallback 的完整行为。

---

## 10. Phase 5：打包、安装版与便携版 artifacts（后续）

Phase 5 对应原 Milestone 7。

当前已确认方向：

- artifacts 需要同时支持安装模式与便携模式。
- 用户应能自由选择安装和使用方式。
- 配置存储使用 OBS `plugin_config`，不写插件安装目录。

需要后续明确：

- Windows installer 形式。
- portable zip 结构。
- macOS / Linux 是否在同一阶段支持。
- CI artifact 命名。
- OBS 31.1.1、32.0、32.1 的验证矩阵。
- 是否需要自动打包资源文件。

---

## 11. Phase 6：性能、稳定性与回归验证（持续 / 后续）

Phase 6 对应原 Milestone 8。

当前已确认原则：

- 多路并发时需要控制 CPU / GPU 占用。
- 可通过降低 Multiview 渲染帧率等方式控制资源占用。
- 窗口不可见或最小化时，OBS 内部信号可以降帧或暂停绘制。
- 外部流不可见策略需后续单独设计。
- 所有异常边界必须限制在插件内部。
- 不得影响 OBS 主线程、主输出或其他插件。
- Qt 窗口句柄与独立渲染管线的实现结论需要纳入性能、稳定性与回归验证。

需要持续补充：

- 默认渲染 FPS。
- 高负载下的降帧策略。
- 多窗口资源占用测试方式。
- 6x6、10x10 场景下的性能目标。
- source 删除、重命名、场景集合切换、OBS 退出等回归测试清单。
- Qt 窗口句柄与独立渲染管线相关的回归测试清单。
- Phase 2 视觉参数开启后的性能基线。

---

## 12. 未来不得误解的事项

以下内容是当前讨论中已经明确不应误解的点：

1. MultiviewManager 不是实际渲染器。
2. MultiviewWindow 负责窗口与用户交互，但不应承载外部流传输状态机。
3. MultiviewRenderer 负责实际画面绘制。
4. Phase 1 只做 OBS 内部源。
5. PGM / PRVW 是 Phase 1 必须支持内容。
6. Source Picker 当前使用列表 + 搜索。
7. 右键菜单中不放“打开 / 聚焦窗口”。
8. 右键菜单中不放“克隆”和“删除”整个 Multiview 实例。
9. 删除整个 Multiview 实例应在管理 / 设置页面中完成。
10. 关闭窗口不等于删除实例配置。
11. 克隆语义是克隆整个 Multiview 实例，并生成新 UUID。
12. 复制网格 UUID 当前不需要；未来如需要，可放到高级 / debug 区域。
13. 布局只允许基础网格 + span，不做像素级微调。
14. 最大网格为 10x10。
15. gutter / border 是 cell 上下左右之间的间距 / 边缘，不仅是左右缝隙。
16. 配置存储使用 OBS `plugin_config`。
17. 按场景集合名区分配置文件。
18. 外部流未来必须支持手动立即重连。
19. 窗口不可见不应简单等同于停止外部流拉流。
20. Layout Preset 采用独立模型，但不保存具体网格信号。
21. 修改布局、添加 / 更换 / 清空 cell 应动态生效，无需重启 OBS。
22. Phase 2 所有视觉参数也必须动态生效。
23. Phase 2 必须采用 Global / Instance / Per Cell 三层视觉设置体系。
24. Phase 2 第一版建议采用分组级继承，而不是字段级继承。
25. Phase 2 必须区分 CellRect 与 SignalRect。
26. 底色 / 底图 / VU meter / 状态 UI 默认基于 CellRect。
27. 安全区 / 内容参考 overlay 默认基于 SignalRect。
28. VU meter 应参考 OBS 内置样式与能力，但第一版不承诺完整复制 OBS mixer。
29. Phase 2 不包含外部流、Signal Lost 完整策略、fallback、installer / portable 正式 artifacts。
30. 尚未详细规划的未来阶段必须在对应阶段完成后再细化。

---

## 13. 当前推荐的下一步

下一步应进入 Phase 2 / Milestone 2.0：

```text
Visual Settings 架构与继承模型
```

建议第一批开发任务：

1. 阅读并理解当前 `src/` 结构，尤其是：
   - `multiview-instance.*`
   - `config-manager.*`
   - `manager-dialog.*`
   - `grid-preview-widget.*`
   - `multiview-window.*`
   - `source-picker.*`
2. 阅读 `docs/phase-2-visual-settings-design.md`。
3. 不实现 label / background / overlay / safe area / VU meter 的完整功能。
4. 先实现 Visual Settings 数据结构与配置读写。
5. 建立 Global / Instance / Cell 三层继承模型。
6. 建立 EffectiveCellVisualSettings 解析逻辑。
7. 保证旧配置兼容。
8. 建立动态刷新通知通路。
9. 编译并确保 Phase 1 行为不回退。

只有 Milestone 2.0 完成后，才进入 Milestone 2.1 标签系统实现。
