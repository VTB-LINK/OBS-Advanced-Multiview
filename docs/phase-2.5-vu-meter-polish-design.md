# Phase 2.5 VU Meter Polish 设计

> 本文档属于 Phase 2.5（M4 收尾 / Phase 3 准备）范围。术语口径见 [TERMINOLOGY.md](TERMINOLOGY.md)。
> Phase 2 / M4 VU meter 主体已实现（开关、位置、anchor、宽度、长度比例、dB 阈值、衰减、flip、track source 等），实现细节见 [phase-2-visual-settings-design.md](phase-2-visual-settings-design.md) 与 [phase-2-hardening-notes.md](phase-2-hardening-notes.md)。
> 本文档记录 Phase 2.5 已完成的 VU meter polish：peak hold、dB 标尺/刻度、Show Labels、Scale Side、以及 scene/source cell 的 trackMode 语义决策。

---

## 1. 范围与边界

### 1.1 Phase 2.5 VU meter polish 的目标

- 让 VU meter 在专业监看场景下信息量更准确（peak hold + dB 标尺）。
- 把 trackMode 在 scene / source cell 上的语义讲清楚，避免在 Phase 3 / M6 接入外部流时出现行为模糊。
- 保持现有 v1 的简洁性，不引入混音器级别的复杂度。

### 1.2 明确不做（与 [known-limitations.md](known-limitations.md) 对齐）

- 完整复制 OBS Mixer 行为（peak hold 时间矩阵、复杂 ballistic 模型、PFL/AFL/混合总线、推子/增益编辑等）。
- 5.1 / 7.1 多声道独立显示。
- 外部信号（NDI / Spout / 媒体流）音频 meter。这些属于 Phase 3（M6）。
- 把 VU meter 升级成音频混音器界面。

### 1.3 兼容性约束

- 现有 `VuMeterSettings` 字段不能破坏式重命名；新字段必须可选并提供安全默认值。
- 配置 `CURRENT_CONFIG_VERSION` 是否升到 v3 取决于改动是否需要迁移逻辑；优先做向后兼容的可选字段加入，不强制升级 version。
- 渲染路径已从 `MultiviewWindow::render_vu_meter()` 拆分到 [src/multiview-window-vu.cpp](../src/multiview-window-vu.cpp)，仍保持同一帧内同步渲染，不引入新线程或信号回调。

---

## 2. Peak Hold

### 2.1 行为定义

- 在当前 displayPeak 之上额外维护一个 `holdPeak` 浮动值。
- `holdPeak` 在峰值出现时立刻锁定到该峰值，在配置的 `peakHoldMs` 时间内保持不变；超时后按 `peakHoldDecay` 速率向下衰减，直到再次被新的峰值冲顶。
- 渲染时在 bar 当前 displayPeak 顶端的位置画一条 1~2 px 宽的细线（与 bar 同方向垂直），颜色按所在 dB 段（绿 / 黄 / 红）取当前段颜色。

### 2.2 数据模型增量

`VuMeterSettings` 新增字段（可选，安全默认）：

| 字段 | 类型 | 默认 | 取值范围 | 备注 |
| --- | --- | --- | --- | --- |
| `peakHoldEnabled` | bool | true | — | 全局开关 |
| `peakHoldMs` | int | 1500 | 100 ~ 5000 | 峰值保持毫秒数 |
| `peakHoldDecayDbPerSec` | double | 11.76 | 1.0 ~ 60.0 | 保持期结束后的衰减速率，默认与 Medium decay 一致 |
| `peakHoldWidthPx` | int | 2 | 1 ~ 4 | 渲染厚度 |

### 2.3 实现要点

- `CellVolmeter` 新增 `holdPeak` + `holdSetAtNs` 两个字段（仅渲染线程读写，不需要锁）。
- `render_vu_meter()` 现有的 ballistic 段落之后插入 hold 逻辑：
  - 若 `peakMax > holdPeak`，更新 `holdPeak = peakMax`、`holdSetAtNs = now`。
  - 否则若 `now - holdSetAtNs > peakHoldMs * 1e6`，按 `peakHoldDecayDbPerSec` 衰减。
- hold 线绘制紧邻当前 displayPeak 的最高点；与 bar 末端相距 0~1 px，避免视觉粘连。
- 当 `peakHoldEnabled == false` 时整段跳过，零开销。

### 2.4 UI 暴露

`CellDisplaySettingsDialog::create_vu_meter_group()`（[src/cell-display-settings-dialog.cpp](../src/cell-display-settings-dialog.cpp)）追加：

- `Peak Hold` checkbox
- `Hold Time (ms)` spinbox
- `Hold Decay (dB/s)` doubleSpinbox
- `Hold Width (px)` spinbox（默认隐藏在高级折叠区，避免对话框过长）

### 2.5 验收要点

- 默认开启，1500 ms 的保持时间观感与广电监看习惯一致。
- 关闭后立即停止绘制 hold 线。
- 高频峰值（每帧都冲顶）下不抖动。
- 长时间静音段不残留 hold（按衰减规则平稳下降至 `-60 dB`）。

---

## 3. dB 标尺 / 刻度

### 3.1 行为定义

- 在 VU bar 的“非 bar 侧”绘制一组刻度线 + 可选数字标签。
- 默认刻度：`-60, -40, -20, -9, 0`（包含 warning/error 阈值默认对齐位置）。
- 刻度线长度与 VU meter 宽度一致，用户调整 meter width 后刻度线同步变宽。
- 刻度线与数字标签始终以全 alpha 白色渲染，保证 Same side 模式下叠在彩色 bar 上方仍可读。
- 刻度数字渲染在对应刻度线正下方，并按 meter 宽度 clamp，最大宽度不超过 VU meter 宽度。

### 3.2 数据模型增量

`VuMeterSettings` 新增字段：

| 字段 | 类型 | 默认 | 取值范围 | 备注 |
| --- | --- | --- | --- | --- |
| `scaleEnabled` | bool | false | — | 默认关闭，避免对现有用户视觉变动 |
| `scaleTicks` | string | `"-60,-40,-20,-9,0"` | CSV，元素范围 -96 ~ 0 | 用户可逗号分隔自定义；解析失败回退默认 |
| `scaleShowLabels` | bool | true | — | 是否绘制数字 |
| `scaleColor` | uint32 (ARGB) | `0x80FFFFFF` | — | 当前仅使用 RGB，渲染时强制 alpha 为 255 |
| `scaleSide` | enum | Auto | `Auto / Same / Opposite` | Same 叠在 bar 上；Auto / Opposite 画在非 bar 侧 |

### 3.3 实现要点

- 在 `MultiviewWindow::render_vu_meter()` 内部渲染刻度线与数字标签。
- `scaleTicks` 使用 CSV 解析；无效 token 被跳过；渲染侧最多处理 20 个 tick；全部无效时回退默认 `-60,-40,-20,-9,0`。
- 数字标签使用 OBS 私有 text source 缓存：Windows 使用 `text_gdiplus`，macOS/Linux 使用 `text_ft2_source_v2`。
- `scaleSide = Same` 时刻度线叠在 bar 内；`Auto / Opposite` 画在非 bar 侧。
- 同样在 `scaleEnabled == false` 时全段跳过。

### 3.4 UI 暴露

`create_vu_meter_group()` 在 Peak Hold 之后追加：

- `Show Scale` checkbox
- `Scale Ticks (dB, CSV)` lineEdit（带占位提示 `-60,-40,-20,-9,0`）
- `Show Labels` checkbox
- `Scale Side` 下拉（`Auto / Same / Opposite`）

`scaleColor` 字段保留在数据模型中，但当前 UI 不暴露颜色选择器；实现上强制全 alpha 白色，以优先保证 Same side 模式的可读性。

### 3.5 验收要点

- 关闭时零渲染开销，零额外日志。
- 自定义刻度 `"-60,-30,-15,-6,0"` 解析正确并按值排序绘制。
- 非法输入（如 `"abc,-20,0"`）跳过无效 token，仍绘制有效 tick。
- 无声时刻度线与标签仍持续渲染，不依赖当前音量 level。
- Same side 模式下刻度线与标签叠在彩色 bar 上方，不被 bar 覆盖。
- 数字标签宽度不超过 meter 宽度。

---

## 4. trackMode 语义决策（Per-cell trackMode）

### 4.1 当前实现回顾

[src/multiview-window.cpp](../src/multiview-window.cpp) `compute_active_track_bit()` 明确注释：

> Track selection is a window-wide knob in v1 (per-cell trackMode override is deferred);
> resolving the full effective settings would only end up reading these same two fields from the instance layer.

也就是说当前 `VuMeterSettings.trackMode` 与 `manualTrackIndex` 虽然是字段级存在于 Global/Instance/Cell 三层结构里，但实际只读 instance 层的值，window-wide 计算一个 active track bit，所有 cell 共享。

### 4.2 各 cell 类型的预期语义（建议 Phase 2.5 决策）

| cell 类型 | 默认 trackMode | 说明 | per-cell override 是否实现 |
| --- | --- | --- | --- |
| PGM | AutoFollowStreaming | 始终代表“观众听到的音”，最符合导播监看直觉 | **不实现**；使用 instance 配置 |
| PRVW | AutoFollowStreaming | 与 PGM 同语义；Studio Mode OFF 时本就 fallback 到 PGM | **不实现**；使用 instance 配置 |
| Scene | 继承 instance | scene 内多 source 按统一 track 过滤；与 PGM 行为一致 | **不实现**；Cell scope 禁用 Track Source / Manual Track |
| Source | 继承 instance | 单 source 多 track 时仍按 instance track 过滤 | **不实现**；Cell scope 禁用 Track Source / Manual Track |

### 4.3 最终设计决策（已落地）

1. **保留 `CellVisualSettings.vuMeter.trackMode` 字段**：不删除，便于未来无破坏式扩展。
2. **保留 `CellVisualSettings.vuMeter.manualTrackIndex` 字段**：同上。
3. **`CellDisplaySettingsDialog` 在 Cell scope 下禁用 Track Source / Manual Track 控件**：明确 trackMode 当前是 instance/window 级，per-cell override deferred。
4. **保留 `compute_active_track_bit()` 现状**：继续从 instance 读取，不引入 per-cell 解析。
5. **如未来需实现 per-cell trackMode**：调整 `compute_active_track_bit()` 为 `compute_active_track_bit_for_cell(int cellIndex)`，并改用 `EffectiveCellVisualSettings.vuMeter.trackMode`；现有调用点（rebuild、polling、scene change）均在渲染线程，改动为本地化。

### 4.4 已关闭的问题

- Scene cell 上是否需要 per-cell trackMode override？
  - **结论：不做。** Scene cell 继承 instance/window 级 trackMode。
- Source cell（绑定单个媒体源 / 浏览器源）上 per-cell trackMode 是否值得实现？
  - **结论：不做。** Source cell 继承 instance/window 级 trackMode；字段保留，未来如有明确混音监看需求再扩展。
- 是否需要在 UI 中显示当前生效的 track（例如 “Active: Track 1”）作为只读提示？
  - **结论：本轮不做。** 现有 UI 通过 Global/Instance scope 的 Track Source / Manual Track 控件表达当前设置。

---

## 5. 实施顺序与里程碑

Phase 2.5 实施结果：

1. **设计审阅**：已完成，见第 4.3 / 4.4 节。
2. **Peak Hold 实现**：已完成（数据模型 + 持久化 + UI + 渲染 + 硬化）。
3. **dB Scale 实现**：已完成（数据模型 + 持久化 + UI + 刻度线 + Show Labels + scaleSide）。
4. **trackMode UI 落地决策**：已完成，Cell scope 禁用 Track Source / Manual Track；未增加 Active Track 标签。
5. **回归验收**：已由用户完成，并已同步到 [phase-2-acceptance-checklist.md](phase-2-acceptance-checklist.md) §2.5。

---

## 6. 与现有文档的引用关系

- 顶层路线：[ROADMAP.md](ROADMAP.md) §7 Milestone 4 Phase 2.5 范围。
- 视觉系统设计基准：[phase-2-visual-settings-design.md](phase-2-visual-settings-design.md) §11 VU Meter v1。
- 硬化与已知观察项：[phase-2-hardening-notes.md](phase-2-hardening-notes.md) 中 VU 相关段落（rebuild_volmeters / collect_audio_sources / 日志可读性补丁等）。
- 验收清单：[phase-2-acceptance-checklist.md](phase-2-acceptance-checklist.md) §2.5 Phase 2.5 polish 子段。
- 已知限制：[known-limitations.md](known-limitations.md) `VU Meter` 段。
- 术语规范：[TERMINOLOGY.md](TERMINOLOGY.md)。
