# OBS Advanced Multiview 开发计划

> 本文件是给项目成员与后续 Copilot session 共同阅读的开发计划。  
> 内容仅基于当前已提供的项目说明、PRD/架构概要、图像材料，以及截至本文件创建时的讨论结论。  
> 对尚未详细讨论的未来阶段，本文只保留方向性占位，并明确标注“需在对应后续阶段详细规划”。

相关参考文档：

- 术语权威基准：[docs/TERMINOLOGY.md](docs/TERMINOLOGY.md)（Phase / Milestone 命名以本文件为准）。
- Phase 2 视觉参数设计：[docs/phase-2-visual-settings-design.md](docs/phase-2-visual-settings-design.md)。
- Phase 2 代码硬化记录：[docs/phase-2-hardening-notes.md](docs/phase-2-hardening-notes.md)。
- Phase 3 Signal Lost 与外部信号设计：[docs/phase-3-signal-lost-and-external-sources-design.md](docs/phase-3-signal-lost-and-external-sources-design.md)。
- Phase 1 验收清单：[docs/phase-1-acceptance-checklist.md](docs/phase-1-acceptance-checklist.md)。
- Phase 1 代码硬化记录：[docs/phase-1-hardening-notes.md](docs/phase-1-hardening-notes.md)。
- 已知限制：[docs/known-limitations.md](docs/known-limitations.md)。
- UI ASCII 线框：[docs/ui-ascii-wireframes.md](docs/ui-ascii-wireframes.md)。

---

## 当前阶段状态

以 [docs/TERMINOLOGY.md](docs/TERMINOLOGY.md) 中固化的术语为准：

| 编号 / Phase | 状态 | 备注 |
| --- | --- | --- |
| M0 ~ M3 / Phase 1 | 功能闭环已完成 | 详见 [docs/phase-1-acceptance-checklist.md](docs/phase-1-acceptance-checklist.md)；OBS 32.0 / tag 构建 / Linux / macOS 运行时尚未完全勾选 |
| M4 / Phase 2 | 主体功能已完成 | Label、Background、SafeArea、Overlay、VU Meter、PGM/PRVW Highlight、三层 Visual Settings、动态生效；详见 [docs/phase-2-visual-settings-design.md](docs/phase-2-visual-settings-design.md) 与 [docs/phase-2-hardening-notes.md](docs/phase-2-hardening-notes.md) |
| Phase 2.5（M4 收尾 / Phase 3 准备） | 已完成 | 文档重基线、验收清单、术语统一、VU meter polish 设计/实现均已完成；不是新的全局 Milestone，仅收尾窗口 |
| M5 / Phase 3 上半段 | 功能完成 | Signal Lost 完整运行时（Black / PlaceholderImage / ClearCell）+ Fallback (PGM/PRVW/Scene/Source/Image) + Reconnect Now + 动态生效；详见 [docs/phase-3-acceptance-checklist.md](docs/phase-3-acceptance-checklist.md) |
| M6 / Phase 3 下半段 | 未开始 | 外部流接入：DistroAV NDI、obs-spout2 Spout、FFmpeg、VLC |
| M7 / M8 / Phase 4 | 未开始 | 打包/安装版/便携版、性能稳定性回归 |

---

## 0. 项目目标与范围

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

---

## 1. 已确认的关键产品决策

### 1.1 管理入口

- 不做 dock 作为第一阶段入口。
- 在 OBS 顶部菜单的“工具”下拉菜单内提供管理 dialog 入口。
- 点击后打开 OBS Advanced Multiview 的管理 / 设置页面。
- 通过管理 / 设置页面打开具体 Multiview 实例。

### 1.2 Multiview 实例与窗口

第一阶段决策：

- 一个 Multiview 实例同一时间只允许一个可编辑窗口。
- 如果用户重复打开同一个实例，则聚焦已有窗口，而不是创建第二个同 UUID 窗口。
- 如果用户需要多个相同或相似的窗口，应在管理页面中克隆整个 Multiview 实例。
- 克隆后生成新的实例 UUID，并可独立编辑、保存和打开。

暂不做：

- 同一个 Multiview 实例打开多个不同步的可编辑窗口。
- 同一个 Multiview 实例多窗口自动同步编辑。
- 多窗口冲突合并。

原因：

- 多窗口同时编辑同一份实例会引入复杂的 dirty 状态、覆盖顺序、保存冲突和同步问题。
- 第一阶段应优先保证行为明确、稳定和可维护。

### 1.3 布局能力

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
- gutter / border 厚度后续按 PRD 支持 0 到 50 px。
- gutter / border 是 cell 上下左右之间的间距 / 边缘，不仅是左右缝隙。

### 1.4 第一阶段信号范围

第一阶段只做 OBS 内部信号：

- PGM。
- PRVW。
- 任意 Scene。
- 任意 Source。

外部信号不属于第一阶段：

- NDI。
- Spout。
- RTMP。
- HLS / M3U8。
- FLV。
- SRT。
- WebRTC。

### 1.5 Source Picker

第一阶段使用：

- 列表 + 搜索。

暂不做：

- OBS 32 风格卡片预览 source picker。
- 实时缩略图网格选择器。

后续可参考 OBS 32 新版 Add Source 交互，但不作为第一阶段要求。

### 1.6 保存语义

保存需要区分：

1. 保存布局。
2. 保存网格信号。
3. 后续如有必要，再考虑保存全部。

已确认：

- “保存布局”和“保存网格信号”必须分开。
- 布局保存主要发生在管理 / 设置 dialog 的当前 Multiview 配置页面。
- 网格信号保存可以从 Multiview 渲染窗口右键菜单触发。
- 管理 / 设置页面也应能看到信号状态，并提供保存网格信号的入口，以避免该能力只隐藏在右键菜单里。

布局保存内容：

- rows。
- columns。
- span 合并结构。
- gutter / border 厚度。
- 与布局相关的默认显示参数。

网格信号保存内容：

- 每个 cell 绑定的信号。
- PGM / PRVW / Scene / Source 引用。
- 空 cell 状态。
- 后续阶段中每个 cell 的标签、背景、overlay、VU meter、fallback、lost signal 策略等。

### 1.7 预设管理

预设管理采用方案 B：单独建模 Layout Preset。

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

与 Multiview 实例的关系：

- Multiview 实例保存自己的布局数据与 cell assignments。
- Multiview 实例可以来源于 Layout Preset。
- 第一阶段仍以 Multiview 实例保存布局为主。
- 第一阶段不要求完成完整的 Layout Preset 管理 UI。
- Layout Preset 的完整创建、命名、保存、复用、删除流程不属于 Phase 2 主体，需在 Phase 3/4 规划时单独排期。

### 1.8 配置存储

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

场景集合名变化时：

- 使用新名字另存新的配置文件。
- 旧文件延迟删除。
- 删除失败不得影响插件运行。

配置写入要求：

- 必须避免写入半截 JSON 导致配置损坏。
- 保存应使用临时文件 + rename 的原子化思路。
- JSON 内必须包含 `configVersion`，以支持后续迁移。

### 1.9 右键菜单

MultiviewWindow 内右键菜单根据右键位置动态显示。

#### 右键已有信号 cell

```text
全屏
窗口置顶
---
更换 / 编辑信号源
清空此格
编辑网格
保存网格信号
---
全局设置
关闭
```

#### 右键空白 cell

```text
全屏
窗口置顶
---
添加 / 选择信号源
编辑网格
保存网格信号
---
全局设置
关闭
```

#### 右键 gutter / 缝隙

```text
全屏
窗口置顶
---
编辑网格
保存网格信号
---
全局设置
关闭
```

右键菜单中不放：

- 打开 / 聚焦窗口。
- 复制网格 UUID。
- 克隆。
- 删除。

这些操作的位置：

- 打开 / 聚焦窗口：管理 / 设置页面的实例列表中。
- 克隆：管理 / 设置页面中，语义为克隆整个 Multiview 实例。
- 删除：管理 / 设置页面中，语义为删除整个 Multiview 实例。
- 复制 UUID：第一阶段不需要；未来如需要，可放到高级 / debug 区域。

### 1.10 动态生效

PRD 要求修改网格配置、增删信号源、更改断流策略等操作实时生效，无需重启 OBS。

当前确认原则：

- Milestone 2 中，编辑网格后的布局变化应尽可能对已打开的当前 MultiviewWindow 动态生效，无需重启 OBS。**（已完成）**
- Milestone 3 中，添加 / 更换 / 清空 cell 后，当前 MultiviewWindow 应动态更新，无需重启 OBS。**（已完成）**
- Milestone 4 中，视觉参数修改应动态生效。**（已完成）** 由 `MultiviewWindow::refresh_visual_settings()` + `notify_multiview_visual_settings_changed()` 触发；详见 [docs/phase-2-visual-settings-design.md](docs/phase-2-visual-settings-design.md) 第 15 节。
- Milestone 5 中，断流策略修改应动态生效；具体规则需在对应后续阶段详细规划。

### 1.11 Qt 窗口句柄与独立渲染管线

PRD 中提到结合 Qt 窗口句柄构建独立硬件加速渲染管线。

当前确认原则：

- 该能力属于 MultiviewWindow 与 MultiviewRenderer 的实现关键点。
- 需要在 Milestone 3 中结合 OBS 31.1.1 到 32.1 的实际 API 与仓库结构确认具体实现方式。
- 需要在 Milestone 8 中补充性能、稳定性与回归验证。
- 当前不在 plan.md 中无依据指定具体渲染上下文实现方式。

### 1.12 外部流重连策略

外部流不属于第一阶段，但已确认未来要求：

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

外部流详细规划需在 Phase 3（M5~M6）中再展开。

---

## 2. 建议的软件分层

### 2.1 MultiviewManager

职责：

- 插件级管理器。
- 管理所有 Multiview 实例。
- 管理全局设置。
- 管理配置加载与保存。
- 管理实例打开、聚焦、关闭、克隆、删除。
- 管理 Layout Preset 数据结构；完整 UI 不属于 Phase 2 主体，需在 Phase 3/4 规划时单独排期。
- 在 OBS 退出时统一关闭窗口并释放资源。
- 负责工具菜单入口打开的管理 / 设置 dialog。

对应产品概念：

- 管理 / 设置页面。
- Multiview 实例列表。
- 全局设置。
- 当前 Multiview 的布局配置入口。
- 后续 Layout Preset 管理入口。

不负责：

- 每帧渲染。
- 具体 OBS source 绘制。
- 外部流传输。

### 2.2 MultiviewInstance / MultiviewModel

职责：

- 表示一个 Multiview 实例的数据模型。
- 保存实例 UUID。
- 保存实例名称。
- 保存 rows / columns。
- 保存 span 布局。
- 保存 cell assignments。
- 保存 dirty 状态。
- 可记录其布局是否来源于某个 Layout Preset。

建议 dirty 状态拆分：

- `layoutDirty`。
- `signalDirty`。
- `windowDirty`。

第一阶段可简化 UI 表达，但内部应保留区分能力。

### 2.3 Layout Preset

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

说明：

- 第一阶段仍以 Multiview 实例保存布局为主。
- Layout Preset 的完整管理流程不属于 Phase 2 主体，需在 Phase 3/4 规划时单独排期。

### 2.4 MultiviewWindow

职责：

- 某个 Multiview 实例的独立 Qt 窗口。
- 窗口化显示。
- 全屏切换。
- 窗口置顶。
- 右键菜单。
- 鼠标 hit-test。
- 判断右键命中已有信号 cell、空白 cell 或 gutter。
- 打开“添加 / 选择信号源”或“更换 / 编辑信号源”对话框。
- 触发清空 cell。
- 触发保存网格信号。
- 打开当前 Multiview 的编辑网格页面。
- 打开全局设置页面。
- 关闭当前窗口。

不建议负责：

- NDI / Spout / 网络拉流传输逻辑。
- 外部流重连状态机。
- 每种信号 provider 的内部生命周期。

### 2.5 SignalProvider / SignalRuntime

职责：

- 抽象每个 cell 的信号来源。
- 第一阶段支持 OBS 内部信号。
- 未来支持外部信号。
- 提供信号状态、显示名称、错误状态等。
- 后续负责外部流连接、断开、重试、fallback 状态。

第一阶段 provider：

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

未来 provider 需随 Phase 3（M5~M6）详细规划。

### 2.6 MultiviewRenderer

职责：

- 实际渲染 Multiview 画面。
- 绘制 cell 背景。
- 绘制 OBS source / scene / PGM / PRVW。
- 绘制黑场。
- 绘制标签。
- 绘制边框 / gutter。
- 绘制选中态 / hover 态。
- 后续绘制 overlay、VU meter、lost signal 占位图、fallback 等。
- 保持画面比例，使用 letterbox / pillarbox。

不负责：

- 保存配置。
- 打开右键菜单。
- 处理外部流连接。

---

## 3. Milestone 0：仓库整理与基础骨架

目标：

- 从当前 obs-plugintemplate 基础状态整理出插件工程骨架。
- 确认构建、加载、菜单入口、配置路径的基础能力。
- 为后续 MultiviewManager / Window / Renderer / Config 分层预留代码结构。

范围：

1. 确认仓库仍以 OBS plugin template 为基础。
2. 确认默认分支和基础构建方式。
3. 确认 OBS 31.1.1 最低版本目标下的构建要求。
4. 确认 OBS 32.0 / 32.1 兼容目标。
5. 添加或整理插件入口。
6. 在 OBS 顶部菜单“工具”中添加管理 dialog 入口。
7. 建立基础命名空间与文件结构。
8. 建立配置目录访问逻辑，使用 OBS `plugin_config`。
9. 建立基础日志与错误处理约定。
10. 建立空的管理 dialog。

验收标准：

- 插件可被 OBS 加载。
- OBS 工具菜单中出现 OBS Advanced Multiview 管理入口。
- 点击入口可打开一个基础管理 dialog。
- 插件能定位到自身 `plugin_config/obs-advanced-multiview/` 目录。
- OBS 退出时插件能正常卸载，不遗留窗口或异常。

暂不包含：

- 实际 multiview 渲染。
- source picker。
- PGM / PRVW / Scene / Source 绑定。
- 外部流。
- artifacts 打包细节。

---

## 4. Milestone 1：配置系统与管理 / 设置 Dialog

目标：

- 建立可保存、可加载、可迁移的配置系统。
- 建立管理 / 设置 dialog 的基础功能。
- 支持 Multiview 实例的创建、重命名、打开 / 聚焦、克隆、删除。
- 在配置模型中预留 Layout Preset。

范围：

1. 定义 JSON 配置结构。
2. JSON 中包含 `configVersion`。
3. 配置按当前 OBS 场景集合名分文件保存。
4. 文件名格式建议为 `settings-<场景集合名>.json`。
5. 文件名需进行非法字符清洗。
6. 场景集合改名时使用新名字另存，旧文件延迟删除。
7. 保存配置时使用临时文件 + rename 的原子化思路。
8. 管理 dialog 显示 Multiview 实例列表。
9. 支持新建 Multiview 实例。
10. 支持重命名实例。
11. 支持克隆整个 Multiview 实例。
12. 支持删除整个 Multiview 实例。
13. 支持打开实例。
14. 如果实例已打开，再次打开时聚焦已有窗口。
15. 支持进入当前实例的编辑网格页面。
16. 支持进入全局设置页面。
17. 配置模型中预留 Layout Preset 列表。
18. 第一阶段不要求完成完整的 Layout Preset 管理 UI。

配置模型至少包含：

- 全局设置。
- Multiview 实例列表。
- Layout Preset 列表。
- 每个实例的 UUID。
- 每个实例的名称。
- 每个实例的 layout 数据。
- 每个实例的 cell assignment 数据。
- 每个 Layout Preset 的 UUID、名称、rows、columns、span、gutter / border。

验收标准：

- 新建实例后可保存到 JSON。
- 重启 OBS 后可重新加载实例列表。
- 重命名实例后可保存。
- 克隆实例后生成新 UUID。
- 删除实例后配置更新。
- 打开已打开实例时聚焦已有窗口，不创建同 UUID 第二窗口。
- 配置保存失败时不得破坏旧配置。
- JSON 结构允许后续加入和读取 Layout Preset。

暂不包含：

- 实际 source 渲染。
- 完整 layout editor。
- 完整 source picker。
- 完整 Layout Preset 管理 UI。
- 外部流。

---

## 5. Milestone 2：布局引擎与编辑网格

目标：

- 实现基础网格 + span 的布局引擎。
- 支持最多 10x10 网格。
- 支持编辑当前 Multiview 的布局。
- 支持保存布局。
- 为后续 Layout Preset 管理提供布局数据基础。

范围：

1. 实现 LayoutEngine。
2. 支持 rows 1 到 10。
3. 支持 columns 1 到 10。
4. 支持 cell span。
5. 校验 span 不超出边界。
6. 校验 cell / region 不重叠。
7. 支持 gutter / border 厚度，范围按 PRD 为 0 到 50 px。
8. gutter / border 需要按 cell 上下左右之间的间距 / 边缘处理。
9. 计算每个 cell / region 的显示 rect。
10. 计算 video rect，保证源比例，使用 letterbox / pillarbox。
11. 支持 hit-test：鼠标位置映射到已有信号 cell、空白 cell、gutter。
12. 管理 / 设置 dialog 中提供编辑网格页面。
13. 编辑网格页面支持调整 rows / columns / span。
14. 编辑网格页面支持保存布局。
15. 修改布局后标记 `layoutDirty`。
16. 已打开的当前 MultiviewWindow 应尽可能动态应用布局变化，无需重启 OBS。
17. Layout Preset 的完整创建、命名、保存、复用、删除流程不属于 Phase 2 主体，需在 Phase 3/4 规划时单独排期；第一阶段仍以 Multiview 实例保存布局为主。

验收标准：

- 可创建 1x1 到 10x10 的布局。
- 可配置 span。
- 无效 span 会被拒绝或安全回退，不导致崩溃。
- 重叠 span 会被拒绝或安全回退。
- 保存布局后重启可恢复。
- hit-test 能区分已有信号 cell、空白 cell、gutter。
- gutter / border 的 hit-test 覆盖 cell 上下左右之间的间距 / 边缘。
- 已打开的当前 MultiviewWindow 能在布局修改后动态更新，无需重启 OBS。

暂不包含：

- source picker 的完整 UI。
- 外部流。
- overlay / VU meter。
- 高级视觉参数。
- 完整 Layout Preset 管理 UI。

---

## 6. Milestone 3：MultiviewWindow 与 OBS 内部信号渲染

目标：

- 实现第一阶段核心可用产品：自定义布局的独立 Multiview 窗口。
- 支持 OBS 内部信号 PGM、PRVW、Scene、Source。
- 支持右键菜单、添加 / 更换信号、清空 cell、保存网格信号。
- 结合 OBS 31.1.1 到 32.1 的实际 API 与仓库结构确认 Qt 窗口句柄和独立渲染管线的具体实现方式。

范围：

1. 实现 MultiviewWindow。
2. 实现 MultiviewRenderer 的第一阶段能力。
3. 支持窗口化显示。
4. 支持全屏。
5. 支持窗口置顶。
6. 支持关闭窗口。
7. 支持右键菜单。
8. 右键已有信号 cell 时显示：
   - 全屏。
   - 窗口置顶。
   - 更换 / 编辑信号源。
   - 清空此格。
   - 编辑网格。
   - 保存网格信号。
   - 全局设置。
   - 关闭。
9. 右键空白 cell 时显示：
   - 全屏。
   - 窗口置顶。
   - 添加 / 选择信号源。
   - 编辑网格。
   - 保存网格信号。
   - 全局设置。
   - 关闭。
10. 右键 gutter / 缝隙时显示：
    - 全屏。
    - 窗口置顶。
    - 编辑网格。
    - 保存网格信号。
    - 全局设置。
    - 关闭。
11. 实现第一版 source picker：列表 + 搜索。
12. source picker 支持选择 PGM。
13. source picker 支持选择 PRVW。
14. source picker 支持选择 Scene。
15. source picker 支持选择 Source。
16. 支持清空 cell。
17. 支持保存网格信号。
18. 修改 cell 绑定后标记 `signalDirty`。
19. 保存网格信号后写入配置。
20. 添加 / 更换 / 清空 cell 后，当前 MultiviewWindow 应动态更新，无需重启 OBS。
21. 渲染时保持源比例，使用 letterbox / pillarbox。
22. OBS 内部源删除或失效时不得崩溃，应显示安全状态。
23. OBS 退出时关闭所有 MultiviewWindow 并释放引用。
24. 确认 Qt 窗口句柄与独立渲染管线的实际实现方式；不得在未验证 API 的情况下假设具体渲染上下文方案。

验收标准：

- 能从工具菜单打开管理 dialog。
- 能创建 Multiview 实例。
- 能编辑 10x10 以内布局。
- 能打开独立 Multiview 窗口。
- 能把 PGM、PRVW、Scene、Source 放入 cell。
- 能清空 cell。
- 添加 / 更换 / 清空 cell 后窗口动态更新，无需重启 OBS。
- 能保存网格信号。
- 重启 OBS 后可恢复 cell 绑定。
- 能全屏和置顶。
- 关闭窗口不删除实例配置。
- 删除 OBS source 时插件不崩溃。
- 对 Qt 窗口句柄与独立渲染管线形成实现结论，并将仍需验证的部分留给 Milestone 8。

暂不包含：

- OBS 32 风格 source card 预览。
- NDI / Spout / 网络流。
- VU meter。
- overlay。
- 底色 / 底图。
- fallback 视频 / 图片。
- 完整 Layout Preset 管理 UI。

完成 Milestone 3 后，才进入后续视觉增强与外部流的详细规划。

---

## 7. Milestone 4：视觉参数与辅助功能

状态：**主体已完成（Phase 2）**；**Phase 2.5（M4 收尾 / Phase 3 准备）已完成**，包括验收、文档同步、VU meter polish 与代码硬化。详见：

- 设计基准：[docs/phase-2-visual-settings-design.md](docs/phase-2-visual-settings-design.md)
- 代码硬化记录：[docs/phase-2-hardening-notes.md](docs/phase-2-hardening-notes.md)

已实现要点（与 PRD 对齐）：

- 三层 Visual Settings（Global / Instance / Per-Cell）+ 分组级继承。
- Label：`None / Overlay / Below` 三种显示模式，`Top / Bottom` 位置，字体 / 字号 / 缩放 / 文本色 / 背景透明度等。
- Background：底色 + 底图（路径），`Fit / Stretch` 两种 fit 模式。
- Overlay：自定义前景图层，支持 `Cell / Signal` anchor 与透明度。
- Safe Area：内置 EBU R95 安全区。
- VU Meter：cell 级开关，位置 `Top / Bottom / Left / Right`，warning / error dB 阈值，三档衰减率，track source 路由。
- Highlight：OBS 原生风格 PGM / PRVW 高亮边框，直接 / 嵌套四态。
- 动态生效：所有视觉参数变更通过 `notify_multiview_visual_settings_changed()` 推送到打开的 MultiviewWindow。

Phase 2.5 已完成内容（不扩张功能）：

- 文档重基线：本文件、[README.md](README.md)、[docs/known-limitations.md](docs/known-limitations.md)、新增 `docs/phase-2-acceptance-checklist.md`。
- 术语统一：以 [docs/TERMINOLOGY.md](docs/TERMINOLOGY.md) 为准，避免 Phase / Milestone 互换。
- VU meter polish 设计与实现：Peak Hold、dB 标尺/刻度、Show Labels、Scale Side、scene/source cell 的 trackMode 语义决策与 Cell scope 禁用。
- 归档 [docs/phase-2-hardening-notes.md](docs/phase-2-hardening-notes.md) 观察项到验收清单或后续 issue。

Phase 2.5 明确不做：

- 不扩展 Label 位置（不加 Left / Right / Center / 角落）。
- 不引入图像资源管理系统（不复制素材到 `plugin_config`，不做资源库）。
- 不做复杂 overlay 图层栈；未来内置 safe-zone 图示走插件 DLL 旁资源文件夹动态读取。
- 不复刻完整 OBS Mixer；5.1 / 7.1 多声道独立 meter 低优先级。
- 不实现完整 Layout Preset 管理 UI（继续保留数据结构）。

---

## 8. Milestone 5：断开 / 删除 / Signal Lost 行为

状态：**功能完成**。详细设计基准见 [docs/phase-3-signal-lost-and-external-sources-design.md](docs/phase-3-signal-lost-and-external-sources-design.md)，验收记录见 [docs/phase-3-acceptance-checklist.md](docs/phase-3-acceptance-checklist.md)。

已落地能力（与 PRD 对齐）：

- 三层 Lost Signal 设置（Global + per-Cell Override；设计上明确不引入 Instance 层）。
- Internal source 删除：`Black + MISSING SOURCE/MISSING SCENE` overlay、`PlaceholderImage`、`ClearCell`（assignment 异步释放，无闪烁）。
- Fallback：OBS 内部 PGM / PRVW / Scene / Source 实时渲染 + 静态图片；FALLBACK 状态横幅。
- Reconnect Now：右键菜单触发，按 `manualReconnectCooldownMs` 冷却，覆盖 internal scene/source 与 PGM/PRVW。
- 动态生效：`notify_multiview_signal_settings_changed(uuid)` 全链接通；不重建整个 display。
- 鲁棒性硬化：source-list 信号桥 lazy 化（消除闪烁）、精准 source_create / source_remove 处理、第三方插件崩溃保护、render 路径双重 `obs_source_removed` guard、VU rebuild 节流、锁顺序纪律、配置 v2 → v3 兼容。
- 文案：`MISSING SOURCE` / `MISSING SCENE` / `FALLBACK` 已上线；`SIGNAL LOST` / `RECONNECTING` 留给 M6。

设计明确不做（M5 范围内）：

- Fallback 失败递归 fallback；fallback chain（多级链）。
- Status overlay 文案多语言（保留为常量；Phase 4 再考虑）。

留给 M6 / 后续阶段：

- 外部源 SignalLost / RetryWithFallback / Reconnecting 状态实际渲染。
- Phase 3 综合硬化 pass（统一审计 internal + external 生命周期）。

---

## 9. Milestone 6：外部流接入

状态：**未开始**。详细设计基准见 [docs/phase-3-signal-lost-and-external-sources-design.md](docs/phase-3-signal-lost-and-external-sources-design.md) §8，执行级拆分写入 [docs/phase-3-acceptance-checklist.md](docs/phase-3-acceptance-checklist.md) 的 M6 章节。

PRD 与已确认策略方向：

- NDI / Spout：不集成 SDK，动态使用宿主环境的 DistroAV / obs-spout2 source type；只创建 / 更新 / 销毁 OBS private source。
- Media (RTMP / HLS / FLV / SRT)：复用 OBS 内置 `ffmpeg_source`；VLC 复用 `vlc_source`，作为可选 provider，不阻塞 M6.1~M6.3 闭环。
- WebRTC：保留 `WebRtcReserved` 占位，不在 M6 实现 runtime。
- Private source 不进入 OBS 场景列表。
- 不使用 while true 空转重试；只在监督层做低频 health check 与 backoff 重建。
- 必须提供 “Reconnect Now”；插件关闭 / 窗口关闭 / OBS 退出时所有 runtime 必须可释放。

M6 子任务编号（与 design doc §14 对齐）：

- M6.0：Provider registry + source type availability detection（含 VU `Auto` / `ExternalSource` 三层语义升级）。
- M6.1：FFmpeg media provider（`ffmpeg_source`，第一个外部 vertical slice）。
- M6.2：DistroAV NDI provider（`ndi_source`），**列表发现必做**：SourcePicker 打开自动扫一次 + 手动 Refresh；不做后台轮询；手动输入是 fallback。
- M6.3：Spout provider（`spout_capture`），**列表发现必做**：策略与 NDI 一致；Spout 无音频按 silence 处理（不进 Lost / 不刷日志）。
- M6.4：VLC provider（`vlc_source`），可选。
- M6.5：WebRTC reserved placeholder（disabled UI + enum/config 占位）。
- M6.6：Phase 3 综合硬化 + 验收 + 跨版本回归（31.1.1 smoke / 32.0 smoke / 32.1.2 full）。

VU meter 三层设置在 M6.0 升级（已确认）：

- 新增 `Auto` 模式：内部 OBS cell 走原有 streaming-track 逻辑；外部 cell 直接对 external private source 计量。
- 新增 `ExternalSource` 手动模式：与 Stream / Track 1..6 区分；强制按外部源自身音频计量。
- `Manual Track 1..6` 保留旧语义。
- 重新启用 cell 级 trackMode override（Phase 2.5 曾推迟）。

风险与原则（继承 PRD / general instructions）：

- DistroAV / obs-spout2 自身已有内部 receiver / reconnect / discovery 逻辑（DistroAV `NDIFinder` 5 秒缓存 + `isRefreshing`；obs-spout2 同步 `GetSenderCount/GetSender`）。我们的监督层不重复造轮子。
- 不能盲目假设 `obs_get_source_properties("ndi_source")` / `obs_get_source_properties("spout_capture")` 在 null data 下安全；需要走真实 / 休眠 private source 实例或受控 helper。
- Render thread 保持 provider-agnostic：不创建、不释放、不重连外部 source。
- 所有外部 source create / update / release 必须在 `source_mutex_` 之外执行，沿用 Phase 2 / M5 的锁顺序纪律。

---

## 10. Milestone 7：打包、安装版与便携版 artifacts

状态：未开始，需在对应后续阶段详细规划。

当前已确认方向：

- artifacts 需要同时支持安装模式与便携模式。
- 用户应能自由选择安装和使用方式。
- 配置存储使用 OBS `plugin_config`，不写插件安装目录。

进入对应后续阶段时需要明确：

- Windows installer 形式。
- portable zip 结构。
- macOS / Linux 是否在同一阶段支持。
- CI artifact 命名。
- OBS 31.1.1、32.0、32.1 的验证矩阵。
- 是否需要自动打包资源文件。

---

## 11. Milestone 8：性能、稳定性与回归验证

状态：未开始，需在对应后续阶段详细规划。

当前已确认原则：

- 多路并发时需要控制 CPU / GPU 占用。
- 可通过降低 Multiview 渲染帧率等方式控制资源占用。
- 窗口不可见或最小化时，OBS 内部信号可以降帧或暂停绘制。
- 外部流不可见策略需后续单独设计。
- 所有异常边界必须限制在插件内部。
- 不得影响 OBS 主线程、主输出或其他插件。
- Qt 窗口句柄与独立渲染管线的实现结论需要纳入性能、稳定性与回归验证。

进入对应后续阶段时需要明确：

- 默认渲染 FPS。
- 高负载下的降帧策略。
- 多窗口资源占用测试方式。
- 6x6、10x10 场景下的性能目标。
- source 删除、重命名、场景集合切换、OBS 退出等回归测试清单。
- Qt 窗口句柄与独立渲染管线相关的回归测试清单。

---

## 12. 未来不得误解的事项

以下内容是当前讨论中已经明确不应误解的点：

1. MultiviewManager 不是实际渲染器。
2. MultiviewWindow 负责窗口与用户交互，但不应承载外部流传输状态机。
3. MultiviewRenderer 负责实际画面绘制。
4. 第一阶段只做 OBS 内部源。
5. PGM / PRVW 是第一阶段必须支持内容。
6. Source picker 第一阶段使用列表 + 搜索。
7. 右键菜单中不放“打开 / 聚焦窗口”。
8. 右键菜单中不放“克隆”和“删除”整个 Multiview 实例。
9. 删除整个 Multiview 实例应在管理 / 设置页面中完成。
10. 关闭窗口不等于删除实例配置。
11. 克隆语义是克隆整个 Multiview 实例，并生成新 UUID。
12. 复制网格 UUID 第一阶段不需要。
13. 布局只允许基础网格 + span，不做像素级微调。
14. 最大网格为 10x10。
15. gutter / border 是 cell 上下左右之间的间距 / 边缘，不仅是左右缝隙。
16. 配置存储使用 OBS `plugin_config`。
17. 按场景集合名区分配置文件。
18. 场景集合改名时使用新名字另存，旧文件延迟删除。
19. 外部流未来必须支持手动立即重连。
20. 窗口不可见不应简单等同于停止外部流拉流。
21. 预设管理采用独立 Layout Preset；但第一阶段仍以 Multiview 实例保存布局为主。
22. Layout Preset 不保存具体网格信号。
23. 修改布局、添加 / 更换 / 清空 cell 应动态生效，无需重启 OBS。
24. Qt 窗口句柄与独立渲染管线的具体实现方式需结合 Milestone 3 的实际实现确认，不应无依据假设。
25. 尚未详细规划的未来阶段必须在对应 Milestone 完成后再细化。

---

## 13. 当前推荐的第一阶段最小可交付闭环

状态：**Phase 1（M0~M3）最小闭环已完成**。本节作为历史基线保留，验收证据见 [docs/phase-1-acceptance-checklist.md](docs/phase-1-acceptance-checklist.md)。

第一阶段最小可交付闭环由 Milestone 0 到 Milestone 3 组成。

完成后应具备：

- OBS 工具菜单入口。
- 管理 / 设置 dialog。
- Multiview 实例列表。
- 新建、重命名、克隆、删除实例。
- 打开 / 聚焦实例窗口。
- 1x1 到 10x10 布局。
- span 合并。
- 保存布局。
- 独立 MultiviewWindow。
- 右键菜单。
- PGM / PRVW / Scene / Source 选择。
- cell 清空。
- 保存网格信号。
- 布局修改后已打开的当前 MultiviewWindow 动态更新。
- 添加 / 更换 / 清空 cell 后当前 MultiviewWindow 动态更新。
- 配置写入 plugin_config。
- 重启 OBS 后恢复配置。
- OBS source 删除时不崩溃。
- OBS 退出时安全释放窗口与引用。

说明：

- Layout Preset 采用独立模型，但完整 Layout Preset 管理 UI 不属于第一阶段最小闭环。
- Qt 窗口句柄与独立渲染管线的具体实现结论需在 Milestone 3 中形成，并在 Milestone 8 中验证。

只有完成这个闭环后，才建议进入视觉增强、断流策略、外部流和打包完善。
