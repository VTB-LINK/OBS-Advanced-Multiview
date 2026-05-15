# OBS Advanced Multiview 开发计划

> 本文件是给项目成员与后续 Copilot session 共同阅读的开发计划。  
> 内容仅基于当前已提供的项目说明、PRD/架构概要、图像材料，以及截至本文件创建时的讨论结论。  
> 对尚未详细讨论的未来阶段，本文只保留方向性占位，并明确标注“需等待 Milestone X 完成后详细规划”。

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

- 最低支持 OBS 31.0.0 / 31.0.x / 31.0。
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

### 1.7 配置存储

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

### 1.8 右键菜单

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

### 1.9 外部流重连策略

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

外部流详细规划需等待 Milestone 3 完成后再展开。

---

## 2. 建议的软件分层

### 2.1 MultiviewManager

职责：

- 插件级管理器。
- 管理所有 Multiview 实例。
- 管理全局设置。
- 管理配置加载与保存。
- 管理实例打开、聚焦、关闭、克隆、删除。
- 在 OBS 退出时统一关闭窗口并释放资源。
- 负责工具菜单入口打开的管理 / 设置 dialog。

对应产品概念：

- 管理 / 设置页面。
- Multiview 实例列表。
- 全局设置。
- 当前 Multiview 的布局配置入口。

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

建议 dirty 状态拆分：

- `layoutDirty`。
- `signalDirty`。
- `windowDirty`。

第一阶段可简化 UI 表达，但内部应保留区分能力。

### 2.3 MultiviewWindow

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

### 2.4 SignalProvider / SignalRuntime

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

未来 provider 需等待 Milestone 3 完成后详细规划。

### 2.5 MultiviewRenderer

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
3. 确认 OBS 31.0.0 最低版本目标下的构建要求。
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

配置模型至少包含：

- 全局设置。
- Multiview 实例列表。
- 每个实例的 UUID。
- 每个实例的名称。
- 每个实例的 layout 数据。
- 每个实例的 cell assignment 数据。

验收标准：

- 新建实例后可保存到 JSON。
- 重启 OBS 后可重新加载实例列表。
- 重命名实例后可保存。
- 克隆实例后生成新 UUID。
- 删除实例后配置更新。
- 打开已打开实例时聚焦已有窗口，不创建同 UUID 第二窗口。
- 配置保存失败时不得破坏旧配置。

暂不包含：

- 实际 source 渲染。
- 完整 layout editor。
- 完整 source picker。
- 外部流。

---

## 5. Milestone 2：布局引擎与编辑网格

目标：

- 实现基础网格 + span 的布局引擎。
- 支持最多 10x10 网格。
- 支持编辑当前 Multiview 的布局。
- 支持保存布局。

范围：

1. 实现 LayoutEngine。
2. 支持 rows 1 到 10。
3. 支持 columns 1 到 10。
4. 支持 cell span。
5. 校验 span 不超出边界。
6. 校验 cell / region 不重叠。
7. 支持 gutter / border 厚度，范围按 PRD 为 0 到 50 px。
8. 计算每个 cell / region 的显示 rect。
9. 计算 video rect，保证源比例，使用 letterbox / pillarbox。
10. 支持 hit-test：鼠标位置映射到已有信号 cell、空白 cell、gutter。
11. 管理 / 设置 dialog 中提供编辑网格页面。
12. 编辑网格页面支持调整 rows / columns / span。
13. 编辑网格页面支持保存布局。
14. 修改布局后标记 `layoutDirty`。

验收标准：

- 可创建 1x1 到 10x10 的布局。
- 可配置 span。
- 无效 span 会被拒绝或安全回退，不导致崩溃。
- 重叠 span 会被拒绝或安全回退。
- 保存布局后重启可恢复。
- hit-test 能区分已有信号 cell、空白 cell、gutter。

暂不包含：

- source picker 的完整 UI。
- 外部流。
- overlay / VU meter。
- 高级视觉参数。

---

## 6. Milestone 3：MultiviewWindow 与 OBS 内部信号渲染

目标：

- 实现第一阶段核心可用产品：自定义布局的独立 Multiview 窗口。
- 支持 OBS 内部信号 PGM、PRVW、Scene、Source。
- 支持右键菜单、添加 / 更换信号、清空 cell、保存网格信号。

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
20. 渲染时保持源比例，使用 letterbox / pillarbox。
21. OBS 内部源删除或失效时不得崩溃，应显示安全状态。
22. OBS 退出时关闭所有 MultiviewWindow 并释放引用。

验收标准：

- 能从工具菜单打开管理 dialog。
- 能创建 Multiview 实例。
- 能编辑 10x10 以内布局。
- 能打开独立 Multiview 窗口。
- 能把 PGM、PRVW、Scene、Source 放入 cell。
- 能清空 cell。
- 能保存网格信号。
- 重启 OBS 后可恢复 cell 绑定。
- 能全屏和置顶。
- 关闭窗口不删除实例配置。
- 删除 OBS source 时插件不崩溃。

暂不包含：

- OBS 32 风格 source card 预览。
- NDI / Spout / 网络流。
- VU meter。
- overlay。
- 底色 / 底图。
- fallback 视频 / 图片。

完成 Milestone 3 后，才进入后续视觉增强与外部流的详细规划。

---

## 7. Milestone 4：视觉参数与辅助功能

状态：需等待 Milestone 3 完成后详细规划。

当前仅保留已讨论和 PRD 中已列明方向：

目标方向：

- 底色。
- 底图。
- 前景 overlay。
- 安全区示意图，例如 EBU R95。
- VU meter 开关。
- 标签名显示模式。

标签名显示模式需支持：

1. 不显示名称。
2. 名称叠加显示。
3. 显示在信号下。

需要等待 Milestone 3 完成后再明确：

- 这些配置放在 cell 级别还是实例默认级别。
- UI 入口具体放置位置。
- overlay 资源存储方式。
- VU meter 具体数据来源和绘制方式。
- 性能限制。
- 与保存网格信号的关系。

---

## 8. Milestone 5：断开 / 删除 / Signal Lost 行为

状态：需等待 Milestone 3 完成后详细规划。

当前仅保留已讨论和 PRD 中已列明方向。

OBS 内部源删除时，未来需支持的方向：

- 显示占位图。
- 保持黑场。
- 清空并释放该网格。
- 如果用户 ctrl+z 恢复，尝试名称匹配。

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

需要等待 Milestone 3 完成后再明确：

- 第一版 lost signal UI。
- 状态机字段。
- fallback 类型。
- 内部源恢复匹配策略。
- 是否按 cell 配置策略。

---

## 9. Milestone 6：外部流接入

状态：需等待 Milestone 3 完成后详细规划；部分内容也依赖 Milestone 5。

当前仅保留已讨论和 PRD 中已列明方向。

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

需要等待 Milestone 3 完成后再明确：

- 如何检测 obs-ndi / DistroAV 插件。
- 如何检测 obs-spout2 插件。
- 如何创建 private source。
- 如何处理不同插件版本 settings schema。
- 外部流 source picker UI。
- 外部流状态机。
- 重试、立即重连、fallback 的完整行为。

---

## 10. Milestone 7：打包、安装版与便携版 artifacts

状态：需等待 Milestone 3 完成后详细规划。

当前已确认方向：

- artifacts 需要同时支持安装模式与便携模式。
- 用户应能自由选择安装和使用方式。
- 配置存储使用 OBS `plugin_config`，不写插件安装目录。

需要等待 Milestone 3 完成后再明确：

- Windows installer 形式。
- portable zip 结构。
- macOS / Linux 是否在同一阶段支持。
- CI artifact 命名。
- OBS 31.0.0、32.0、32.1 的验证矩阵。
- 是否需要自动打包资源文件。

---

## 11. Milestone 8：性能、稳定性与回归验证

状态：需等待 Milestone 3 完成后详细规划。

当前已确认原则：

- 多路并发时需要控制 CPU / GPU 占用。
- 可通过降低 Multiview 渲染帧率等方式控制资源占用。
- 窗口不可见或最小化时，OBS 内部信号可以降帧或暂停绘制。
- 外部流不可见策略需后续单独设计。
- 所有异常边界必须限制在插件内部。
- 不得影响 OBS 主线程、主输出或其他插件。

需要等待 Milestone 3 完成后再明确：

- 默认渲染 FPS。
- 高负载下的降帧策略。
- 多窗口资源占用测试方式。
- 6x6、10x10 场景下的性能目标。
- source 删除、重命名、场景集合切换、OBS 退出等回归测试清单。

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
15. 配置存储使用 OBS `plugin_config`。
16. 按场景集合名区分配置文件。
17. 场景集合改名时使用新名字另存，旧文件延迟删除。
18. 外部流未来必须支持手动立即重连。
19. 窗口不可见不应简单等同于停止外部流拉流。
20. 尚未详细规划的未来阶段必须在对应 Milestone 完成后再细化。

---

## 13. 当前推荐的第一阶段最小可交付闭环

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
- 配置写入 plugin_config。
- 重启 OBS 后恢复配置。
- OBS source 删除时不崩溃。
- OBS 退出时安全释放窗口与引用。

只有完成这个闭环后，才建议进入视觉增强、断流策略、外部流和打包完善。
