# Phase 2 Visual Settings Design

> 本文件用于定义 OBS Advanced Multiview 的 Phase 2 设计。  
> Phase 2 聚焦视觉参数系统与 cell 显示能力，不包含外部流、Signal Lost 完整策略、安装器 / 便携版正式分发系统等后续阶段内容。  
> 本文件是 `ROADMAP.md`、`docs/ui-ascii-wireframes.md`、`docs/phase-1-acceptance-checklist.md`、`docs/known-limitations.md` 和 Phase 1 基线实现之上的详细设计文档。

---

## 1. Phase 2 目标

Phase 1 已完成基础可用产品闭环：

- OBS 工具菜单入口；
- 管理 / 设置 Dialog；
- Multiview 实例管理；
- 配置保存与恢复；
- 基础网格与 span 布局；
- MultiviewWindow；
- Source Picker；
- OBS 内部信号 PGM / PRVW / Scene / Source；
- 基础右键菜单；
- Phase 1 验收、hardening 与版本基线整理。

Phase 2 的目标不是扩展到外部流，而是把 Multiview 的视觉表现能力做完整，形成稳定的三层视觉参数系统，并在现有 MultiviewWindow / Renderer 基础上支持：

- global setting；
- instance level setting；
- per cell setting；
- label / 标签显示系统；
- 底色；
- 底图；
- 前景 overlay；
- 安全区；
- VU meter；
- 动态生效；
- 配置保存与恢复；
- 渲染坐标空间统一；
- 旧配置兼容。

Phase 2 的核心不是单独实现某一个视觉功能，而是先建立 **可扩展的视觉参数架构**。如果没有统一的三层 setting 体系和坐标空间，后续每增加一个视觉功能都会让 UI、配置结构和 Renderer 进一步碎片化。

---

## 2. Phase 2 非目标

Phase 2 明确不包含以下内容：

- NDI；
- Spout；
- RTMP；
- HLS / M3U8；
- FLV；
- SRT；
- WebRTC；
- Signal Lost 完整策略；
- fallback 图片 / 视频；
- 外部流 backoff 重试；
- 外部流立即重连；
- 完整 Layout Preset 管理 UI；
- 安装器 / 便携版正式 artifact 流程；
- 完整发布系统；
- 外部流 private source 详细实现。

这些内容属于 `ROADMAP.md` 中的 Milestone 5、Milestone 6、Milestone 7、Milestone 8 或更后续阶段。

---

## 3. Phase 2 核心原则

### 3.1 三层 setting 体系

Phase 2 必须统一采用以下设置层级：

```text
Global Visual Settings
        ↓
Instance Visual Settings
        ↓
Per Cell Visual Settings
```

语义：

- **Global Visual Settings**：整个插件的视觉默认值。
- **Instance Visual Settings**：某个 Multiview 实例级别的视觉设置，可继承 Global，也可覆盖 Global。
- **Per Cell Visual Settings**：某个 cell 级别的视觉设置，可继承 Instance，也可覆盖 Instance。

### 3.2 继承优先级

某个 cell 的最终生效视觉配置应按以下顺序解析：

```text
Per Cell override
  fallback to Instance setting
  fallback to Global setting
  fallback to hardcoded safe default
```

### 3.3 不采用简单的“整对象继承开关”

Phase 2 不建议仅通过以下方式实现：

```text
instance.inheritGlobal = true/false
cell.inheritInstance = true/false
```

因为后续字段会越来越多，用户很可能只想对某些视觉参数覆盖，而对其他参数保持继承。如果只能整对象继承，则灵活性不足，后续 UI 也会失衡。

### 3.4 分组级继承优先

Phase 2 第一版建议采用 **分组级继承**，而不是一开始做每个字段都可单独继承。

即将视觉参数拆成几个逻辑组，每组单独支持：

```text
Inherit
Override
```

这样可以在保持灵活性的同时避免 UI 过于碎片化。

### 3.5 动态生效

Phase 2 所有视觉参数修改都应遵守 Phase 1 已确认的原则：

```text
修改后当前 MultiviewWindow 动态更新，无需重启 OBS
```

### 3.6 保持鲁棒性优先

Phase 2 不允许为了视觉效果牺牲稳定性。所有新增能力必须满足：

- 不影响 OBS 主线程、主输出或其他插件；
- 不引入 busy loop / 空转；
- 不引入未受控的资源生命周期问题；
- 不因图片加载、字体、音频 meter 等失败导致崩溃；
- 不破坏既有 PGM / PRVW / Scene / Source 生命周期管理；
- 不破坏既有 Phase 1 配置与渲染行为。

---

## 4. 建议的视觉参数分组

Phase 2 建议把视觉参数分为以下几个组：

```text
VisualSettings
├─ BackgroundSettings
├─ LabelSettings
├─ SafeAreaSettings
├─ VuMeterSettings
└─ OverlaySettings
```

### 4.1 BackgroundSettings

负责：

- 底色；
- 底图；
- 背景相关 fit / fill 行为；
- 透明源下方背景表现。

### 4.2 LabelSettings

负责：

- 标签名显示模式；
- 文本位置；
- 字体；
- 字号；
- 字号缩放模式；
- 文本颜色；
- 文本背景透明度；
- 文本背景圆角等。

### 4.3 SafeAreaSettings

负责：

- 安全区开关；
- 安全区类型（例如 EBU R95）；
- 安全区颜色与透明度。

### 4.4 VuMeterSettings

负责：

- VU meter 开关；
- VU meter 位置；
- VU meter 透明度；
- VU meter 样式。

### 4.5 OverlaySettings

负责：

- 前景 overlay；
- overlay 图片路径；
- overlay 透明度；
- overlay 的锚点模式；
- overlay 的 fit mode。

---

## 5. 推荐的数据模型

以下为推荐的数据模型方向。该设计用于指导实现，名称和结构可在编码阶段略作调整，但语义不应改变。

```text
VisualSettings
├─ BackgroundSettings
│  ├─ colorEnabled
│  ├─ color
│  ├─ imageEnabled
│  ├─ imagePath
│  └─ imageFitMode
│
├─ LabelSettings
│  ├─ displayMode
│  ├─ position
│  ├─ fontFamily
│  ├─ fontSize
│  ├─ fontScaleMode
│  ├─ minFontSize
│  ├─ maxFontSize
│  ├─ textColor
│  ├─ backgroundOpacity
│  ├─ backgroundRounded
│  └─ margin
│
├─ SafeAreaSettings
│  ├─ enabled
│  ├─ preset
│  ├─ color
│  └─ opacity
│
├─ VuMeterSettings
│  ├─ enabled
│  ├─ position
│  ├─ opacity
│  ├─ width
│  └─ style
│
└─ OverlaySettings
   ├─ enabled
   ├─ imagePath
   ├─ opacity
   ├─ fitMode
   └─ anchorMode
```

### 5.1 Global / Instance / Cell 的组织形式

建议：

```text
GlobalVisualSettings
InstanceVisualSettings
CellVisualSettings
EffectiveCellVisualSettings
```

其中：

- `GlobalVisualSettings`：实际保存全局视觉默认值。
- `InstanceVisualSettings`：包含每个视觉组是否继承 global。
- `CellVisualSettings`：包含每个视觉组是否继承 instance。
- `EffectiveCellVisualSettings`：运行时解析结果，不直接持久化。

### 5.2 分组级继承示意

建议每组使用以下语义：

```text
InheritanceMode:
  Inherit
  Override
```

例如：

```text
InstanceVisualSettings
  Background: Override
  Label: Inherit
  SafeArea: Inherit
  VuMeter: Override
  Overlay: Inherit
```

以及：

```text
CellVisualSettings
  Background: Inherit
  Label: Override
  SafeArea: Inherit
  VuMeter: Override
  Overlay: Inherit
```

第一版先做组级继承即可，不建议一开始为每个字段都提供独立继承开关。

---

## 6. Label / 标签系统设计

Phase 2 中，标签系统是优先级最高的视觉功能之一，因为：

- 它在 PRD 中已有明确要求；
- 它最能验证三层 setting 体系；
- 它会影响 Renderer 的坐标与布局组织；
- 它会为后续背景、overlay、安全区、VU meter 提供统一绘制顺序基础。

### 6.1 LabelDisplayMode

必须支持：

```text
None
Overlay
Below
```

对应 PRD 术语：

- 不显示名称；
- 名称叠加显示；
- 显示在信号下。

### 6.2 None

行为：

- 不绘制标签。
- 视频可使用完整的内容区域。
- 不额外占用 cell 空间。

### 6.3 Overlay

行为：

- 标签叠加在画面上方或下方；
- 默认不改变 video rect；
- 更接近 OBS 内置 multiview；
- 需要支持文本背景透明度、圆角、字体、字号、颜色等。

第一版建议：

- Overlay 标签默认锚定在 `CellRect`；
- 不引入复杂的 label anchor 选择 UI；
- 后续如有必要，再扩展为 `Cell` / `Signal` 可切换。

### 6.4 Below

行为：

- 在 cell 底部预留独立 label 区域；
- 视频区域缩小，不遮挡视频；
- 比 Overlay 模式更像导播台布局。

计算顺序应为：

```text
CellRect
  -> LabelRect（位于下方）
  -> ContentRect = CellRect - LabelRect
  -> SignalRect = 将 signal fit 到 ContentRect
```

而不是先算 `SignalRect` 再硬贴标签。

### 6.5 LabelSettings 字段建议

建议包括：

```text
displayMode
position
fontFamily
fontSize
fontScaleMode
minFontSize
maxFontSize
textColor
backgroundOpacity
backgroundRounded
margin
```

### 6.6 字号缩放模式

建议：

```text
Fixed
ScaleWithCell
```

语义：

- `Fixed`：固定字号，例如 14 px；
- `ScaleWithCell`：字号随 cell 尺寸变化；
- 必须配合 `minFontSize` 与 `maxFontSize`，避免在 10x10 或极大 span 时失控。

### 6.7 文本位置

长期建议支持：

```text
Top
Bottom
Left
Right
TopLeft
TopRight
BottomLeft
BottomRight
Center
```

但第一版建议只做：

```text
Top
Bottom
Below
```

以降低 UI 与 Renderer 复杂度。

---

## 7. Background / 底色与底图设计

### 7.1 背景职责边界

底色和底图应视为 **cell 背景**，而不是 signal 内容的一部分。

因此它们默认应基于：

```text
CellRect
```

而不是 `SignalRect`。

### 7.2 BackgroundSettings 建议字段

```text
colorEnabled
color
imageEnabled
imagePath
imageFitMode
```

第一版可以允许更简化，例如不使用 `Enabled` 字段，而用“空路径 / 默认颜色”表示未启用状态。

### 7.3 底色

适用场景：

- 透明 Scene；
- 透明 Source；
- 未来透明外部流；
- 空 cell；
- letterbox / pillarbox 情况下的背景呈现。

### 7.4 底图

适用场景：

- 透明内容下方背景图；
- 空 cell 背景图；
- 品牌化背景；
- 参考纹理。

### 7.5 底图路径策略

第一版建议：

- 保存绝对路径；
- 文件不存在时记录日志并优雅回退；
- 不将图片主动复制到 `plugin_config`；
- 不做资源管理器复杂导入流程。

理由：

- Phase 2 目标是建立视觉参数系统，不是做资源资产管理系统；
- 避免在 Phase 2 引入不必要的便携版资源同步复杂度；
- 后续如需要，可再扩展资源复制或 project-local asset 模式。

### 7.6 底图 fit mode

长期建议：

```text
Fit
Fill
Stretch
Tile
```

第一版建议只做：

```text
Fit
```

必要时再补 `Stretch`。

---

## 8. CellRect 与 SignalRect 坐标空间设计

这是 Phase 2 的关键基础。

如果不正式区分这两个坐标空间，后续 overlay、safe area、label、VU meter 在 letterbox / pillarbox 情况下会不断互相冲突。

### 8.1 CellRect

定义：

```text
当前 cell 的完整显示区域，不包括 gutter / border。
```

用于：

- 底色；
- 底图；
- cell hover / selection；
- VU meter；
- 状态 UI；
- Signal Lost UI；
- label 的 Below 模式；
- 某些 cell 级装饰性 overlay。

### 8.2 SignalRect

定义：

```text
source 经过 letterbox / pillarbox 后，实际视频画面所在区域。
```

用于：

- 实际视频绘制；
- 安全区；
- 内容参考类 overlay；
- 跟随画面比例的前景图；
- 需要贴合视频内容边界的辅助线。

### 8.3 第一版默认规则

建议：

| 功能 | 默认 anchor | 原因 |
|---|---|---|
| 底色 | CellRect | cell 背景 |
| 底图 | CellRect | cell 背景 |
| 视频画面 | SignalRect | letterbox / pillarbox 后实际画面 |
| 安全区 | SignalRect | 与画面内容相关 |
| 前景参考 overlay | SignalRect | 与画面内容相关 |
| Overlay 标签 | CellRect | 布局稳定，更接近 OBS multiview |
| Below 标签 | CellRect | 需要占用 cell 下方区域 |
| VU meter | CellRect | 属于监看 UI |
| 状态 UI / Signal Lost | CellRect | 属于监看 UI |

### 8.4 是否开放 anchor mode

Phase 2 设计上建议预留：

```text
OverlayAnchorMode:
  Cell
  Signal
```

但第一版不必对所有元素都开放该选项。

建议：

- 安全区固定基于 `SignalRect`；
- VU meter 固定基于 `CellRect`；
- 底色 / 底图固定基于 `CellRect`；
- 自定义前景 overlay 可在后续版本中支持选择 `Cell` / `Signal`。

---

## 9. Safe Area / 安全区设计

### 9.1 语义

安全区属于与视频内容相关的参考信息，而不是 cell 背景或 UI 装饰，因此应基于：

```text
SignalRect
```

### 9.2 第一版范围

第一版建议只做一个 preset：

```text
EBU R95
```

避免一开始做过多标准切换。

### 9.3 SafeAreaSettings 建议字段

```text
enabled
preset
color
opacity
```

### 9.4 绘制规则

- 在 `SignalRect` 内绘制；
- 不改变 video rect；
- 不绘制到 letterbox / pillarbox 的黑边上；
- 与标签、overlay、VU meter 叠加顺序必须清晰。

---

## 10. Overlay / 前景 Overlay 设计

### 10.1 两类 overlay 需要区分

#### A. 内容参考类 overlay

例如：

- 安全区；
- 构图辅助线；
- 校准图层；
- 参考边界。

这类 overlay 应基于：

```text
SignalRect
```

#### B. UI 装饰类 overlay

例如：

- VU meter；
- 状态角标；
- Signal Lost 状态；
- 某些 cell 级提示元素。

这类 overlay 应基于：

```text
CellRect
```

### 10.2 OverlaySettings 建议字段

```text
enabled
imagePath
opacity
fitMode
anchorMode
```

### 10.3 第一版建议

第一版可以先实现：

- 自定义前景 overlay 图片；
- anchor 默认为 `SignalRect`；
- fit mode 默认 `Fit`；
- 文件不存在时安全回退；
- 不做资源复制。

---

## 11. VU Meter 设计

### 11.1 设计原则

VU meter 应尽量参考 OBS 内置样式与能力，而不是从零设计一套全新逻辑。

但 Phase 2 不应承诺一开始完全复制 OBS mixer 的全部行为。

应先做：

```text
VU Meter v1
```

目标是：

- 稳定；
- 可用；
- 风格接近 OBS；
- 不干扰主音频逻辑；
- 不因 meter 数据不可用而崩溃。

### 11.2 VU meter 的默认锚点

建议默认基于：

```text
CellRect
```

原因：

- VU meter 属于 multiview 监看 UI，而不是视频内容；
- 如果基于 `SignalRect`，在 letterbox / pillarbox 时 meter 会缩进黑边，导致多 cell 对齐不稳定；
- 基于 `CellRect` 更符合监看设备中仪表属于 cell UI 的习惯。

### 11.3 VuMeterSettings 建议字段

```text
enabled
position
opacity
width
style
```

位置长期建议：

```text
Left
Right
Bottom
```

第一版建议只做：

```text
Right
```

### 11.4 参考 OBS 内置实现

Phase 2 的 VU meter 需要先研究：

- OBS 内置 mixer 的样式；
- 音频电平接口；
- Scene / Source 的音频能力来源；
- PGM / PRVW 的处理方式；
- peak / magnitude / clipping 的获取方式；
- 多 cell 同时开 meter 时的性能影响。

### 11.5 第一版不承诺的内容

Phase 2 不承诺第一版就做到：

- 完整复制 OBS mixer 的全部交互；
- 多声道复杂显示；
- 所有 source 类型都与 OBS 内置 mixer 完全一致；
- 高级 peak hold 或 dB 标尺配置。

---

## 12. UI 入口设计

Phase 2 不能把所有视觉参数只放在右键菜单里，否则会进一步加重发现成本与维护成本。

### 12.1 Global Visual Settings

建议放在：

```text
管理 / 设置 Dialog
  -> Settings tab
    -> Global Visual Settings
```

至少应包括：

- global label defaults；
- global background defaults；
- global safe area defaults；
- global VU meter defaults；
- global overlay defaults（如有）。

### 12.2 Instance Visual Settings

建议放在：

```text
管理 / 设置 Dialog
  -> 当前 Multiview 实例详情页
    -> Instance Visual Settings
```

需要支持：

- 该组是否继承 global；
- 若 override，则显示该组实际编辑控件；
- 变更后自动保存并动态生效。

### 12.3 Per Cell Visual Settings

建议提供一个独立对话框：

```text
Cell Display Settings Dialog
```

理由：

- 避免把 manager 右侧面板做得过重；
- 可从右键菜单与管理页面共同复用；
- 后续字段较多，独立 dialog 更便于扩展。

建议入口：

```text
MultiviewWindow 右键菜单
  -> Cell Display Settings...
```

以及：

```text
管理 / 设置 Dialog
  -> 当前 Multiview 实例详情
  -> 选中某个 cell 后打开 Cell Display Settings...
```

### 12.4 继承 UI 原则

不要为每个字段都直接显示一排继承 checkbox。第一版建议按组显示：

```text
Background: [Inherit Instance / Override]
Label: [Inherit Instance / Override]
Safe Area: [Inherit Instance / Override]
VU Meter: [Inherit Instance / Override]
Overlay: [Inherit Instance / Override]
```

如果选择 `Override`，再展开该组具体字段。

---

## 13. Renderer 绘制顺序建议

Phase 2 必须明确统一绘制顺序，否则不同视觉元素叠加会越来越难维护。

建议顺序：

```text
1. cell base background
2. custom background color
3. custom background image
4. video render (SignalRect)
5. content-related overlay / safe area (SignalRect)
6. label (Overlay or Below)
7. VU meter (CellRect)
8. cell status UI / future Signal Lost UI (CellRect)
9. hover / selection / debug overlay
```

### 13.1 Below 模式的特殊性

`Below` 模式不是简单在视频下方贴一层 label，而是需要先从 `CellRect` 中划出 `LabelRect`，然后再为 `SignalRect` 计算剩余内容区域。

### 13.2 Overlay 模式的优先级

第一版建议：

- Label Overlay 在 safe area 之后绘制；
- 避免安全区遮住标签；
- 但如果后续有专业用户希望标签低于 overlay，可再扩展层级设置。

---

## 14. 配置持久化与兼容策略

### 14.1 配置兼容目标

Phase 2 必须保证：

- Phase 1 旧配置可正常加载；
- 缺少 visual settings 时能正常 fallback 到默认值；
- 不因为 visual settings 新增字段导致配置损坏或读取失败；
- 动态保存不破坏原子化策略。

### 14.2 `configVersion` 策略

如果 Phase 2 只是在现有结构上新增可选字段，理论上可以不强制升级 `configVersion`。

但若为了三层 visual settings 或 cell visual config 引入了明显结构变化，建议升级：

```text
CURRENT_CONFIG_VERSION = 2
```

并在 load 逻辑中保留：

- 旧结构读取；
- 默认值回填；
- 新结构写回。

### 14.3 auto-save 保持不变

Phase 2 延续当前实际产品行为：

- 修改视觉参数后自动保存；
- 当前 MultiviewWindow 动态刷新；
- 不要求用户手动点击“Save Cell Assignments”类按钮。

---

## 15. 动态更新行为

Phase 2 所有视觉参数变更都必须在当前打开的窗口中动态生效。

至少应覆盖：

- label mode 切换；
- 字号变化；
- 字体变化；
- 文字颜色变化；
- 文字背景透明度变化；
- 底色变化；
- 底图切换；
- 安全区开关；
- VU meter 开关；
- overlay 切换；
- instance level setting 切换；
- cell override 开关切换。

动态更新必须满足：

- 不要求关闭窗口重开；
- 不要求重启 OBS；
- 不破坏 source 引用；
- 不导致窗口闪烁或明显掉帧；
- 不因图片或字体加载失败而阻塞主流程。

---

## 16. Phase 2 milestone 划分

Phase 2 建议拆分为以下 7 个 milestone。

### Milestone 2.0：Visual Settings 架构与继承模型

目标：

- 建立 global / instance / per cell 三层视觉设置；
- 建立 visual settings 数据模型；
- 建立 effective config 解析；
- 建立 JSON schema；
- 建立动态更新通路。

范围：

- `GlobalVisualSettings`；
- `InstanceVisualSettings`；
- `CellVisualSettings`；
- `EffectiveCellVisualSettings`；
- 分组级继承；
- auto-save；
- 旧配置兼容。

验收标准：

- 旧配置正常加载；
- 缺字段正常 fallback；
- instance 可继承 global；
- cell 可继承 instance；
- effective config 计算正确；
- 修改后当前 MultiviewWindow 动态刷新；
- 不破坏 Phase 1 行为。

### Milestone 2.1：Label Settings / 标签系统

目标：

- 实现标签名显示模式；
- 实现字体、字号、缩放模式、文字颜色、背景透明度、背景圆角等；
- 完成 label 的 instance / cell 继承体系。

范围：

- `None / Overlay / Below`；
- `fontFamily`；
- `fontSize`；
- `fontScaleMode`；
- `minFontSize / maxFontSize`；
- `textColor`；
- `backgroundOpacity`；
- `backgroundRounded`；
- 基础位置设置。

验收标准：

- 三种模式正常；
- Fixed / ScaleWithCell 正常；
- 10x10、span cell 正常；
- 重启恢复；
- 动态生效。

### Milestone 2.2：Background Settings / 底色与底图

目标：

- 实现底色；
- 实现底图基础能力；
- 支持透明源背景与空 cell 背景。

范围：

- `backgroundColor`；
- `backgroundImagePath`；
- `backgroundImageFitMode`；
- instance / cell 继承。

验收标准：

- 底色正常；
- 底图正常；
- 图片不存在不崩；
- 透明源表现正确；
- 动态生效；
- 重启恢复。

### Milestone 2.3：Overlay 坐标空间与前景 overlay

目标：

- 建立 `CellRect / SignalRect` 坐标空间；
- 实现前景 overlay；
- 支持 overlay 锚点模式。

范围：

- `CellRect`；
- `SignalRect`；
- `OverlayAnchorMode`；
- 前景 overlay 的路径、透明度、fit mode。

验收标准：

- Signal anchored overlay 在 letterbox / pillarbox 下行为正确；
- Cell anchored overlay 在 cell 区域内行为正确；
- span cell 正常；
- 动态生效。

### Milestone 2.4：Safe Area / 安全区

目标：

- 实现安全区开关；
- 第一版支持 EBU R95；
- 安全区基于 SignalRect 绘制。

范围：

- `safeAreaEnabled`；
- `safeAreaPreset`；
- `safeAreaColor`；
- `safeAreaOpacity`。

验收标准：

- 安全区只在实际画面区域内；
- 不画到黑边；
- 动态生效；
- 重启恢复。

**实现建议（基于 OBS 原生 multiview 分析）**：

OBS 原生使用 `InitSafeAreas()` + `RenderSafeAreas()` 的 vertex buffer 方案：

- 预计算安全区矩形坐标（Action Safe 90%、Title Safe 80%），写入 `gs_vertbuffer_t`；
- 渲染时只需 `gs_load_vertexbuffer` + `gs_draw(GS_LINESTRIP, 0, 0)`，开销极低；
- 关键参考：`obs-studio/UI/multiview.cpp` 中 `InitSafeAreas()`（根据 actionSize/titleSize 计算 inner rect）和 `RenderSafeAreas()`（设 thick_color/thin_color 后 draw linestrip）。

推荐实现路径：

1. 在 `MultiviewWindow` 中添加 `gs_vertbuffer_t *safe_area_vb_` 成员；
2. 布局变更时重建 vertex buffer（与 cell 尺寸同步）；
3. `render()` 中 source 绘制后、label 绘制前调用安全区渲染；
4. 使用 `gs_render_start(true)` / `gs_render_save()` 创建 linestrip geometry；
5. 支持 EBU R95 (Action 93%, Title 90%) 和 SMPTE RP 218（Action 90%, Title 80%）。

### Milestone 2.5：VU Meter v1

目标：

- 参考 OBS 内置 VU meter 样式与能力；
- 实现基础 cell VU meter。

范围：

- `vuMeterEnabled`；
- `vuMeterPosition`；
- `vuMeterOpacity`；
- `vuMeterStyle`；
- 最小可用音频 meter 通路。

验收标准：

- 有音频 source 正常显示；
- 无音频 source 安全隐藏或空表；
- 删除 source 不崩；
- 多 cell 情况下性能可接受；
- 动态生效；
- 重启恢复。

### Milestone 2.6：Visual Settings UI 整合

目标：

- 将 global / instance / cell 视觉设置整合到 UI；
- 避免所有功能只藏在右键菜单中。

范围：

- Settings tab 中的 Global Visual Settings；
- instance detail 中的 Instance Visual Settings；
- Cell Display Settings Dialog；
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

- Global / Instance / Cell 都有清晰入口；
- 继承关系清晰；
- 动态生效；
- auto-save；
- 不阻塞 OBS；
- VU Meter 所有扩展设置在 UI 中可配置并实时生效。

**实现建议（基于 OBS 原生 multiview 分析）**：

1. **使用 OBS 主题变量而非硬编码颜色**：
   - OBS 内部通过 `obs_frontend_get_current_theme()` 和 palette 获取主题色；
   - 插件 UI 中的辅助文字/标签已改用 `QPalette::PlaceholderText`（commit 11151f1）；
   - 后续 Visual Settings UI 中的颜色选择器默认值应从当前主题反映；
   - 参考 OBS `window-basic-main.cpp` 中 `QApplication::palette()` 的用法。

2. **dirty-flag + HookWidget 模式**：
   - OBS 的 Settings Dialog 使用 `HookWidget()` 宏自动监听控件变更，设置 dirty flag；
   - 当 dirty=true 时才触发 save / apply，避免每次控件值变化都写磁盘；
   - 推荐在 Visual Settings 页面中采用类似模式：
     ```cpp
     bool visual_settings_dirty_ = false;
     // 每个控件 connect → visual_settings_dirty_ = true;
     // Apply 按钮或 focusOut/tab切换 时才 flush
     ```
   - 这对 per-cell 设置尤为重要：10x10 grid 有 100 个 cell，不能每次 spinbox 值变化都 save+rebuild。

3. **继承关系 UI 表达**：
   - 对 "inherit" 状态的控件，建议视觉上降低对比度（类似 disabled 但可点击）；
   - 点击 "inherit" 控件时切换为 "override" 模式并恢复完全对比度；
   - 参考 OBS Filter 面板的 "inherit from parent" checkbox 表达方式。

### Milestone 2.7：Phase 2 回归与性能验收

目标：

- 对 Phase 2 全部视觉能力做统一回归；
- 确认 0.2.x baseline。

范围：

- 配置迁移；
- 旧配置兼容；
- 动态更新；
- 多窗口；
- 多显示器；
- OBS 退出；
- source 删除；
- 10x10；
- span；
- 高 DPI；
- CI；
- 文档更新。

验收标准：

- OBS 31.1.1 正常；
- OBS 32.1.2 正常；
- 1x1 / 4x4 / 5x5 / 10x10 正常；
- 多窗口正常；
- source 删除不崩；
- 动态生效稳定；
- 性能可接受；
- 文档同步完成。

---

## 17. Phase 2 验收矩阵建议

Phase 2 完成时，建议至少验证：

```text
OBS 31.1.1
OBS 32.1.2
Windows portable
Windows installed / user plugin dir
Debug
RelWithDebInfo
Release
1x1 / 4x4 / 5x5 / 10x10
span cell
多窗口
多显示器
source 删除 / undo / re-resolve
高 DPI
全屏 / 置顶
```

---

## 18. 建议的实现顺序

Phase 2 不建议并行开发多个视觉功能。推荐严格按以下顺序推进：

```text
Milestone 2.0：Visual Settings 架构与继承模型
Milestone 2.1：Label Settings / 标签系统
Milestone 2.2：Background Settings / 底色与底图
Milestone 2.3：Overlay 坐标空间与前景 overlay
Milestone 2.4：Safe Area / 安全区
Milestone 2.5：VU Meter v1
Milestone 2.6：Visual Settings UI 整合
Milestone 2.7：Phase 2 回归与性能验收
```

其中：

- 不要先做 VU meter；
- 不要先做复杂图片资产管理；
- 不要在 Phase 2 中混入外部流；
- 不要打乱三层设置系统的建立顺序。

---

## 19. 建议的代码组织方向

Phase 2 可能涉及的核心文件：

```text
src/multiview-instance.hpp / .cpp
src/config-manager.hpp / .cpp
src/multiview-window.hpp / .cpp
src/manager-dialog.hpp / .cpp
src/grid-preview-widget.hpp / .cpp
```

可能新增的文件：

```text
src/cell-display-settings-dialog.hpp
src/cell-display-settings-dialog.cpp
```

如果 Renderer 逻辑继续增大，也可以在合适时机考虑：

```text
src/multiview-renderer.hpp
src/multiview-renderer.cpp
```

但不建议在 Phase 2 一开始就进行大重构；应先保证功能与坐标语义清晰，再评估是否拆分 Renderer。

---

## 20. 结论

Phase 2 的本质不是“把若干视觉功能堆上去”，而是：

1. 建立 global / instance / per cell 三层视觉设置系统；
2. 建立统一的 visual settings 数据模型；
3. 建立 `CellRect / SignalRect` 坐标空间；
4. 先完成 label / background / overlay / safe area / VU meter 的可扩展框架；
5. 保持 Phase 1 的稳定性、动态生效能力和配置兼容性；
6. 为后续 Milestone 5 / 6 打下统一的 cell presentation 基础。

只有这样，OBS Advanced Multiview 才能在不破坏既有稳定性的前提下，进入更专业的视觉监看产品阶段。
