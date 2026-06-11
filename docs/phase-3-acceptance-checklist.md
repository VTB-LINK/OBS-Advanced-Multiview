# Phase 3 / M5 验收清单

> 本文档记录 Phase 3 上半段（M5：Signal Lost 与删除行为）的功能完成度与人工验证结果。
> 术语口径以 [docs/TERMINOLOGY.md](docs/TERMINOLOGY.md) 为准；M5 子任务编号对应
> [docs/phase-3-signal-lost-and-external-sources-design.md](docs/phase-3-signal-lost-and-external-sources-design.md) §7。
> M6（外部流接入）尚未开始；硬化 pass 计划在 M6 完成后作为 Phase 3 综合收尾统一进行。

图例：

- `[x]` — 代码已实现 + 已人工验证。
- `[o]` — 代码已实现，仅在 Windows RelWithDebInfo（OBS 31.1.1 + 32.1.2 portable）单机验证，跨平台 / 跨配置未确认。
- `[ ]` — 代码未实现，或属于 M6 / Phase 4 后续任务。
- `~~[ ]~~ **明确不做**` — 设计决策不实现，仅供历史追溯。

---

## M5.0 运行时层 (Signal Runtime State)

- [x] `SignalRuntimeState` 状态机定义于 [src/multiview-window.hpp](src/multiview-window.hpp)：`Empty / Resolving / Active / MissingInternal / Connecting / Lost / RetryScheduled / FallbackActive / Error`
- [x] 每 cell 的 `CellSource` 持有 `state / last_active_ns / last_reconnect_ns / retry_attempt`
- [x] 每 cell 缓存 `effective_lost`（Global + Cell Override 解析结果），避免每帧查 instance config
- [x] `LostSignalSettings` 结构（[src/multiview-instance.hpp](src/multiview-instance.hpp)）持久化字段：
  - `internalMissingBehavior`、`externalLostBehavior`、`placeholderImagePath`、`signalLostImagePath`
  - `fallbackType`（白名单：image / pgm / prvw / scene / source）+ `fallbackName`
  - `placeholderImageFitMode` / `signalLostImageFitMode` / `fallbackImageFitMode`
  - `retryInitialMs / retryMaxMs / manualReconnectCooldownMs`（clamp 落入 [docs/phase-3-signal-lost-and-external-sources-design.md](docs/phase-3-signal-lost-and-external-sources-design.md) §10 边界）
- [x] `CellLostSignalSettings`（含 `InheritanceMode`）持久化；只持久化 Override 条目
- [x] `CURRENT_CONFIG_VERSION` 升到 v3，旧 v2 配置加载时透传默认值；记录升级日志
- [x] `resolve_effective_lost_signal(global, cell)` 实现 Global + Cell 两层（设计明确不引入 Instance 层）

## M5.1 内部 source missing

### 行为分支

- [x] **Black + Missing Source overlay**：默认行为；cell 黑场 + 顶部 / 中部状态横幅
- [x] **Placeholder Image**：用户选择的静态图片（路径持久化绝对路径，与 bg-image 一致）
- [x] **Clear Cell（释放 assignment）**：自动从 `cellAssignments` 移除，立即写盘；ManagerDialog 单元列表同步刷新

### 渲染管线

- [x] 状态分类发生在 render 路径 newState 计算块（[src/multiview-window.cpp](src/multiview-window.cpp) `render()`）
- [x] PGM / PRVW 永远 Active；其它类型在 weak ref 解析失败 / `obs_source_removed()` 命中时进入 MissingInternal
- [x] Lost-Signal image 与 bg-image 走同样的 4 阶段加载（snapshot under lock → disk IO → graphics ops → install）：[src/multiview-window-lost-image.cpp](src/multiview-window-lost-image.cpp)
- [x] 单 slot 自动切换：`compute_wanted_lost_image_path()` 按 `cs.state` + `effective_lost` 决定 placeholder / fallback static image
- [x] 图像 fit 模式独立可配置：`placeholderImageFitMode` / `fallbackImageFitMode` / `signalLostImageFitMode`，默认 Stretch（贴满 cell）

### ClearCell 异步路径

- [x] `on_source_being_removed` 检测 `internalMissingBehavior == ClearCell` → 收集 (row,col) 并设 `cs.pending_clear`
- [x] `QTimer::singleShot(0, this, ...)` 把 `apply_clear_cell_for_rowcols` 投递回 Qt 主线程
- [x] 主线程异步路径执行 `cellAssignments.erase` + `config_->save()` + `refresh_sources()`
- [x] 闪烁抑制：render / fallback resolve / lost-signal image / status overlay 均跳过 `cs.pending_clear` cell；`newState = Empty`
- [x] `pending_clear` 在 `refresh_sources()` 重建 `cell_sources_` 时自然清零

### 文案区分

- [x] `MISSING SCENE`：`cs.type == "scene"` 时显示
- [x] `MISSING SOURCE`：其它非 scene、非 PGM/PRVW 类型显示
- [x] PGM / PRVW 永远不进入此路径

### 验证

- [o] 删除已绑定 scene → 立即 MISSING SCENE，无延迟、无 ghost 渲染
- [o] 删除已绑定非 scene source（image / dshow / browser 等）→ 立即 MISSING SOURCE
- [o] PlaceholderImage 模式：放图 → 删 source → 立即贴满 cell；Fit 模式切换正确
- [o] ClearCell 模式：删 source → cell 立即变 Empty（无 MISSING 闪一帧）→ ManagerDialog 列表同步移除条目 → 重启 OBS 不复现

## M5.2 Status Overlay（状态横幅）

### 已实现

- [x] 共享 text source 池（每个 kind 一个 OBS private text source）：`status_missing_source_` / `status_missing_scene_` / `status_fallback_`
- [x] Windows: `text_gdiplus`，Mac/Linux: `text_ft2_source_v2`
- [x] 横幅高度 clamp（24..64 px ≈ cellH × 0.18），居中
- [x] 小 cell（< 80×32 px）跳过绘制
- [x] `MISSING SOURCE`：深灰半透明
- [x] `MISSING SCENE`：深灰半透明
- [x] `FALLBACK`：暖琥珀色（fallback active 时）

### 留给 M6

- [ ] `SIGNAL LOST`（外部源 Lost / Error 状态）
- [ ] `RECONNECTING`（外部源 Connecting / RetryScheduled 状态）

## M5.3 Reconnect Now

- [x] cell 右键菜单 "Reconnect Now"
- [x] enabled 条件：state ∈ { MissingInternal / Lost / Connecting / RetryScheduled / FallbackActive / Error }
- [x] 冷却：默认 1000 ms，使用 `effective_lost.manualReconnectCooldownMs`；冷却期内重复点击仅 LOG_DEBUG
- [x] PGM / PRVW：仅记录冷却时间戳，立即 Active
- [x] Internal Scene / Source：drop 旧 weak ref + `obs_get_source_by_name`；对 marked-removed source 拒绝 rebind

## M5.4 Fallback

### 已实现

- [x] OBS 内部 PGM / PRVW / Scene / Source 实时渲染替代信号
- [x] Fallback 静态图片（与 PlaceholderImage 共用 `LostSignalImage` slot）
- [x] FALLBACK 状态横幅
- [x] `FallbackActive` 状态在 Reconnect Now eligibility 中正确处理
- [x] `obs_source_removed()` 双重防护（resolve 时 + `obs_source_video_render` 前），跨 fallback 路径生效
- [x] 主信号恢复后自动切回（`update_source_refs` / `update_source_refs_lazy` / 渲染 lazy re-resolve 三条路径协同）

### 设计明确不做（M5 范围内）

- ~~[ ]~~ **fallback 失败递归 fallback**：fallback 自身失败时直接展示 MISSING / SIGNAL LOST，不级联
- ~~[ ]~~ **fallback chain（多级链）**：第一版只支持单一 fallback

### 留给 M6

- [ ] 外部 media URL 作为 fallback
- [ ] NDI / Spout fallback

## M5.5 动态生效

- [x] `notify_multiview_signal_settings_changed(uuid)` 入口（[src/plugin-main.cpp](src/plugin-main.cpp)）
- [x] Settings tab → Edit Global Signal Lost Settings 后调用
- [x] Cell 右键 → Signal Lost Settings 后调用
- [x] 窗口响应 `refresh_signal_settings()`：重算 `effective_lost`、重建 lost-signal image，不重建整个 display
- [x] 不影响其它 cell 的 source refs / VU / 渲染管线

## 鲁棒性 / 硬化（M5 已落地的稳态保障）

- [x] **闪烁修复**：source-list 信号桥改用 `refresh_sources_lazy()`，不再 release+rebuild label/bg/overlay/VU；闪烁完全消除
- [x] **精准 source_remove handler**：通过 calldata 直接拿到 source 指针，命中 cell 同步置 MissingInternal
- [x] **精准 source_create handler**：恢复路径同步 rebind（Edit → Undo Delete 后无 50 ms 延迟）
- [x] **第三方插件崩溃保护**：`on_source_being_removed` 不调 `obs_source_dec_showing`，避免 hide 信号扰动其它插件（streamdeck-plugin-obs 在 source_remove 期间崩溃链路已规避）
- [x] **render 防护**：`obs_source_removed()` 在 srcHolder resolve 后 + `obs_source_video_render` 前各检一次
- [x] **VU rebuild 节流**：≥250 ms 一次（~4 Hz），抑制反复删除/恢复带音频源场景的 attach/detach 风暴
- [x] **lock order**：image rebuild 始终在 source_mutex_ 释放后调用，避免与 graphics lock ABBA
- [x] **配置降级**：v3 → v2 加载兼容；未知 fallbackType 透传 ""（disabled）

## 已知边界与限制

- **Duplicate Scene / Duplicate Sources**：OBS Studio Mode 启用 Duplicate 后，Preview 中删除的 source 在 Program 仍持有副本时不会触发 `source_remove`。我方按 OBS source identity 语义解析，cell 状态 Active 是符合预期的；只有当 Program 也释放最后一个引用时才进入 MissingInternal。这是 OBS 内置语义，不属于本插件需要修复的范围。如需"PGM tree 不可达即 missing"，属于"逻辑可见性"特性，不在 M5 范围。
- **OBS 自身 undo/redo 释放路径**：`obs_source_release+0x3d ← undo_stack::redo` 与 `obs_scene_release+0x45 ← OBSBasic::RemoveSelectedScene` 两条 OBS 内部崩溃链路，本插件不在栈链上。我方已在自身路径全面避免持有 / 渲染 marked-removed source；进一步追责需上游 OBS / 第三方插件配合。
- **placeholder / fallback static image fit**：默认 Stretch（贴满 cell）。Fit 模式保留长宽比但有 letterbox 黑边。FillCrop 暂不实现（与 bg-image 当前不支持 FillCrop 一致；属未来扩展）。
- **status overlay 多语言**：当前文案为英文常量（`MISSING SOURCE` / `MISSING SCENE` / `FALLBACK`），未走 OBS locale。M6 / Phase 4 再考虑。
- **placeholder 图片在多个 instance / 多 window 下不共享纹理**：每个 MultiviewWindow 独立 4 阶段加载。同一图片若被多 instance 引用会重复加载。属性能优化项，目前用例下数量级很小（几张图），不做。

## 下一步

- M6（外部流接入）：DistroAV NDI、obs-spout2 Spout、FFmpeg、VLC provider 设计与实现
- 综合硬化 pass：M6 完成后统一审计内部 + 外部 source 生命周期、锁顺序、退出释放路径、跨平台
- macOS / Linux 运行时验证：M5 全部用例需在 macOS / Linux 上至少跑一次
- OBS 32.0 验证：当前已在 31.1.1 + 32.1.2 验证；未在 32.0 上跑
