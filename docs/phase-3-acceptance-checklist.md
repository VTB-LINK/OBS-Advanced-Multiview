# Phase 3 / M5 验收清单

> 本文档记录 Phase 3 上半段（M5：Signal Lost 与删除行为）的功能完成度与人工验证结果。
> 术语口径以 [TERMINOLOGY.md](TERMINOLOGY.md) 为准；M5 子任务编号对应
> [phase-3-signal-lost-and-external-sources-design.md](phase-3-signal-lost-and-external-sources-design.md) §7。
> M6（外部流接入）尚未开始；硬化 pass 计划在 M6 完成后作为 Phase 3 综合收尾统一进行。

图例：

- `[x]` — 代码已实现 + 已人工验证。
- `[o]` — 代码已实现，仅在 Windows RelWithDebInfo（OBS 31.1.1 + 32.1.2 portable）单机验证，跨平台 / 跨配置未确认。
- `[ ]` — 代码未实现，或属于 M6 / Phase 4 后续任务。
- `~~[ ]~~ **明确不做**` — 设计决策不实现，仅供历史追溯。

---

## M5.0 运行时层 (Signal Runtime State)

- [x] `SignalRuntimeState` 状态机定义于 [src/multiview-window.hpp](../src/multiview-window.hpp)：`Empty / Resolving / Active / MissingInternal / Connecting / Lost / RetryScheduled / FallbackActive / Error`
- [x] 每 cell 的 `CellSource` 持有 `state / last_active_ns / last_reconnect_ns / retry_attempt`
- [x] 每 cell 缓存 `effective_lost`（Global + Cell Override 解析结果），避免每帧查 instance config
- [x] `LostSignalSettings` 结构（[src/multiview-instance.hpp](../src/multiview-instance.hpp)）持久化字段：
  - `internalMissingBehavior`、`externalLostBehavior`、`placeholderImagePath`、`signalLostImagePath`
  - `fallbackType`（白名单：image / pgm / prvw / scene / source）+ `fallbackName`
  - `placeholderImageFitMode` / `signalLostImageFitMode` / `fallbackImageFitMode`
  - `retryInitialMs / retryMaxMs / manualReconnectCooldownMs`（clamp 落入 [phase-3-signal-lost-and-external-sources-design.md](phase-3-signal-lost-and-external-sources-design.md) §10 边界）
- [x] `CellLostSignalSettings`（含 `InheritanceMode`）持久化；只持久化 Override 条目
- [x] `CURRENT_CONFIG_VERSION` 升到 v3，旧 v2 配置加载时透传默认值；记录升级日志
- [x] `resolve_effective_lost_signal(global, cell)` 实现 Global + Cell 两层（设计明确不引入 Instance 层）

## M5.1 内部 source missing

### 行为分支

- [x] **Black + Missing Source overlay**：默认行为；cell 黑场 + 顶部 / 中部状态横幅
- [x] **Placeholder Image**：用户选择的静态图片（路径持久化绝对路径，与 bg-image 一致）
- [x] **Clear Cell（释放 assignment）**：自动从 `cellAssignments` 移除，立即写盘；ManagerDialog 单元列表同步刷新

### 渲染管线

- [x] 状态分类发生在 render 路径 newState 计算块（[src/multiview-window.cpp](../src/multiview-window.cpp) `render()`）
- [x] PGM / PRVW 永远 Active；其它类型在 weak ref 解析失败 / `obs_source_removed()` 命中时进入 MissingInternal
- [x] Lost-Signal image 与 bg-image 走同样的 4 阶段加载（snapshot under lock → disk IO → graphics ops → install）：[src/multiview-window-lost-image.cpp](../src/multiview-window-lost-image.cpp)
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

- [x] `SIGNAL LOST`（外部源 Lost / Error 状态）— 红色 SIGNAL LOST band，由 sticky display state 锁定（M6.6）
- [x] `RECONNECTING`（外部源 Connecting / RetryScheduled 状态）— 改名为 `CONNECTING...`（首次连接 / 重连共用）

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

- [x] 外部 media URL 作为 fallback — 通过 `RetryWithFallback` + image fallback 实现（M6.6 sticky display state）
- [x] NDI / Spout fallback — 同上路径，外部 cell M5 fallback 链路已接通

## M5.5 动态生效

- [x] `notify_multiview_signal_settings_changed(uuid)` 入口（[src/plugin-main.cpp](../src/plugin-main.cpp)）
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

- ~~M6（外部流接入）：DistroAV NDI、obs-spout2 Spout、FFmpeg、VLC provider 设计与实现~~ → **已完成**（见下方 M6 清单）
- 综合硬化 pass：M5 + M6 锁顺序 / 退出释放 / 跨平台 audit 完成；详见 M6.6 章节
- macOS / Linux 运行时验证：M5 全部用例需在 macOS / Linux 上至少跑一次
- OBS 32.0 验证：当前已在 31.1.1 + 32.1.2 验证；未在 32.0 上跑

---

# Phase 3 / M6 验收清单（功能完成）

> M6（外部流接入）功能闭环已完成；M6.6 综合硬化已落地。剩余项为跨平台 / 跨 OBS 版本的人工回归（OBS 32.0、macOS、Linux），不阻塞功能交付。
> 与 [phase-3-signal-lost-and-external-sources-design.md](phase-3-signal-lost-and-external-sources-design.md) §8 / §14 / §15 对齐。
> 每个子项的状态使用与 M5 章节相同的图例：`[x] / [o] / [ ] / ~~[ ]~~`。

## M6.0 Provider Registry + 配置 / 运行时基础

### 配置层

- [x] `SignalProviderType` 枚举与字符串持久化映射：`internal_pgm` / `internal_prvw` / `internal_scene` / `internal_source` / `ffmpeg` / `ndi` / `spout` / `vlc` / `webrtc_reserved`
- [x] `SignalConfig` struct：`provider` + `displayName` + `providerSettings`（OBSData object）
- [x] `CellAssignment` 扩展：保留 `type` / `name` 兼容字段；新增 optional `signalConfig`
- [x] M5 v3 配置加载兼容（不丢字段、不破坏 internal cell 行为）
- [x] 新版本号 / 升级日志（必要时）

### 运行时层

- [x] `CellSource` 演进：新增 private source strong ref、provider type、settings fingerprint、last health timestamp、last dimensions、last error reason、next retry timestamp
- [x] 统一 release helper：layout 缩容、ClearCell、源切换、provider recreate、窗口关闭、OBS 退出走同一路径
- [x] Render 线程 provider-agnostic：不创建 / 释放 / 重连外部 source

### Provider Registry

- [x] `src/signal-provider.hpp` / `.cpp` 接口骨架：`id` / `display_name` / `is_available` / `unavailable_reason` / `defaults` / `build_settings` / `create_or_update` / `release` / `reconnect` / `health`
- [x] `obs_source_get_display_name(source_id) != nullptr` 作为可用性首选检测路径
- [x] Provider 不可用时 UI disabled + reason，不隐藏 tab 不崩溃
- [x] 内部 OBS provider adapter：包装现有 PGM / PRVW / Scene / Source 行为，不立即重写 M5 路径

### VU Meter 三层语义升级（M6.0 必做）

- [x] `VuMeterTrackMode` 扩展：`Auto`、`ExternalSource`、`Manual` 同时存在；旧值持久化兼容
- [x] `Auto`：内部 cell 走原 streaming-track 逻辑；外部 cell 直接对 external private source 计量
- [x] `ExternalSource`：与 Stream / Track 1..6 区分；内部 cell 安全降级 / UI 禁用
- [x] `Manual Track 1..6`：保留旧语义
- [x] cell 级 trackMode override 重新启用（Phase 2.5 推迟项）
- [x] Spout audio 按 silence（极低 / 负无穷 dB）处理；不进 Lost / 不刷日志

### 锁顺序与稳定性

- [x] Provider create / update / release 不持有 `source_mutex_`
- [x] 不在 render 回调内创建 / 重连 / 释放外部 source
- [x] 不在 source-list 信号 handler 内创建外部 source

## M6.1 FFmpeg Media Provider

- [x] Source id：`ffmpeg_source`
- [x] Settings 默认：`input` / `is_local_file=false` / `reconnect_delay_sec=10` / `buffering_mb=2` / `restart_on_activate=true` / `close_when_inactive=false` / `clear_on_media_end=true` / `linear_alpha=false`
- [x] Private source 命名：`OBS Advanced Multiview/<instance>/<row>,<col>/ffmpeg`，不进入 OBS 场景列表
- [x] Health 映射：playing + valid dimensions → `Active`；opening / buffering → `Connecting`；error / ended / 持续零分辨率 → `Lost` / `Error`
- [x] Reconnect Now：先 `obs_source_media_restart`，必要时 recreate
- [x] 状态 overlay：`Connecting` / `RetryScheduled` → `RECONNECTING`；`Lost` / `Error` → `SIGNAL LOST`
- [x] `ExternalLostBehavior` 全部联通：`SignalLostOverlay` / `RetryOnly` / `RetryWithFallback` / `SignalLostImage`
- [x] 主信号恢复后自动切回，释放 fallback ref
- [x] 验证：HLS/M3U8、invalid URL、网络中断、Reconnect Now、fallback、recovery、clear cell、layout 缩容、close window
- [x] Private source 隔离验证（不出现在 OBS 场景 / source 列表）
- [x] **持久化往返验证**：bind external cell → save → 关 OBS → 重开 → cell 自动加载、私有源重新激活、播放恢复（M6.1 中曾因 `type.empty()` 过滤导致外部 cell 在磁盘上丢失，已修；后续 provider 上线必须同样验收此项）

## M6.1+ First-slice 落地后追加任务

详见 [ROADMAP.md §9.1](ROADMAP.md)。M6.2 / step 10 之前必须完成。

### 9.1.A Cell-level 增量重建

- [x] `MultiviewWindow::refresh_cell(int row, int col)` 仅触动该 cell 的 runtime
- [x] 调用方：`on_add_source` / `on_change_source` / `on_clear_cell` / `Edit Source...` 保存路径
- [x] cell 数量改变（layout / Edit Grid）必须仍走全量 `refresh_sources()`
- [x] 私有源 inc_active / dec_active 仍在 `source_mutex_` 之外
- [x] volmeter 通过既有 throttle 标志合并，不另开新机制
- [x] `rebuild_label_sources` / `rebuild_bg_images` / `rebuild_overlay_images` / `rebuild_lost_signal_images` 在路径不变时是 no-op
- [x] 验证：两个 cell 都绑长分片 m3u8 → 改其中一个 cell URL → 另一个 cell 不中断

### 9.1.B Media tab 完整 ffmpeg 能力（local + advanced）

- [x] `Local File` 复选框切换 URL 输入框 ↔ 文件选择器
- [x] 常用区：URL / Local File / Reconnect Delay / Network Buffering / Hardware Decoding / Color Range / Looping
- [x] 高级折叠区（默认折叠，QGroupBox checkable）：Restart on activate（锁定 true）/ Close when inactive（锁定 false）/ Clear on media end / Linear Alpha / Seekable / Speed Percent / FFmpeg Options
- [x] 所有键以原 ffmpeg_source key 名透传 `signalConfig.providerSettings`
- [x] `restart_on_activate=true` 与 `close_when_inactive=false` provider 层强制覆盖，不允许通过 ffmpeg_options 间接绕过
- [x] 验证：网络流（HLS / RTMP）/ 本地文件 / 本地文件 looping / hw decode 切换 / color range / ffmpeg_options 自定义参数

### 9.1.C 通用 Edit Source… 右键菜单

- [x] 仅外部 cell 显示该项；内部 cell（pgm/prvw/scene/source）不显示
- [x] 对话框由 provider 自己构造表单；本次只实现 FFmpeg 版本
- [x] 与 9.1.B 表单复用同一 builder
- [x] 保存：写回 `inst->cellAssignments` 对应 cell 的 `signalConfig.providerSettings` → `config_->save()` → `refresh_cell()`
- [x] 取消 / 关闭对话框不留半保存状态
- [x] 验证：改 URL / 改 buffering / 切换 local file / 加 ffmpeg_options → 仅当前 cell 重连

## M6.2 DistroAV NDI Provider

- [x] Source id：`ndi_source`
- [x] Settings 支持 / 保留：`ndi_source_name` / `ndi_behavior` / `ndi_behavior_timeout` / `ndi_bw_mode` / `ndi_sync` / `ndi_framesync` / `ndi_recv_hw_accel` / `ndi_fix_alpha_blending` / `yuv_range` / `yuv_colorspace` / `latency` / `ndi_audio`
- [x] **列表发现必做**：SourcePicker NDI tab 打开时扫一次；Refresh 按钮触发再扫一次；不后台轮询
- [x] 安全发现路径：不直接 `obs_get_source_properties` null data；通过真实 / 休眠 private source 实例或受控 helper
- [x] 手动输入作为 fallback / advanced（非主路径）
- [x] DistroAV 缺失 → tab disabled + reason
- [x] Private source 命名 `OBS Advanced Multiview/<instance>/<row>,<col>/ndi`
- [x] Health：width/height > 0 + recent video → Active；空分辨率 → Connecting / Lost
- [x] Reconnect 首选 recreate；fallback `obs_source_update`
- [x] 不链接 NDI SDK，不 include DistroAV 头文件，不调 DistroAV 内部函数
- [x] 验证：valid name / invalid name / DistroAV missing / source appears later / recover / 释放路径

## M6.3 Spout Provider

- [x] Source id：`spout_capture`
- [x] Settings：`spoutsenders` / `usefirstavailablesender` / `tickspeedlimit` / `compositemode`
- [x] **列表发现必做**：SourcePicker Spout tab 打开时扫一次；Refresh 按钮触发再扫一次；不后台轮询
- [x] 安全发现路径：同 NDI；通过真实 / 休眠 private source 实例或受控 helper
- [x] 手动输入 + first available 作为 fallback（非主路径）
- [x] obs-spout2 缺失 → tab disabled + reason
- [x] Private source 命名 `OBS Advanced Multiview/<instance>/<row>,<col>/spout`
- [x] Health：dimensions > 0 → Active；sender 缺失 → Lost / Error 不刷屏
- [x] Reconnect 首选 `obs_source_update`，fallback recreate
- [x] 不链接 Spout SDK，不调 obs-spout2 内部函数
- [x] Spout VU = silence（不进 Lost / 不刷日志）
- [x] 验证：first-available / named sender missing / appears / disappears / composite / alpha / 释放路径

## M6.4 VLC Provider（可选）

- [x] Source id：`vlc_source`
- [x] 最小 settings：URL / playlist / restart on activate
- [x] 不阻塞 M6.1 / M6.2 / M6.3 闭环；可后置
- [x] Provider 不可用时 UI disabled + reason

## M6.5 WebRTC Reserved

- [x] `WebRtcReserved` 枚举 / config 占位
- [x] SourcePicker 显示 disabled placeholder + 原因
- [x] 文档记录后续待确认项（transport / signaling / auth / codec / threading）

## M6.6 Phase 3 综合硬化与验收

- [x] M5 回归矩阵全绿：MISSING SCENE / SOURCE、ClearCell 无闪、fallback PGM / static image、Reconnect Now、undo / restore、Duplicate Scene / Sources 边界
- [x] M6 回归矩阵：provider create / invalid config / lost / reconnect / fallback / recover / clear cell / layout 缩容 / close window / OBS exit
- [x] 资源释放：清空 cell / 切换 provider / 缩容 10x10→1x1 / 关闭窗口 / OBS 退出，private source 全部释放
- [x] 锁顺序与生命周期 audit：无 `source_mutex_` 期间的 OBS 长操作；无 render 期间的 provider 操作
- [o] 跨版本：OBS 31.1.1 smoke ✅ / 32.0 smoke ⏳（未跑） / 32.1.2 full provider validation ✅
- [x] 文档：本清单更新、`README.md` / `ROADMAP.md` 状态、`docs/known-limitations.md` 同步

### M6.6 额外硬化（设计 doc 落地后追加）

- [x] sticky `fallback_latched` 跨 retry 周期保持 fallback 不闪烁；只在 supervisor 报 Active 时清除
- [x] 所有 `ExternalLostBehavior` 都触发 retry；行为差异仅控制 overlay（SignalLostOverlay/Image → red SIGNAL LOST；RetryWithFallback → yellow FALLBACK；RetryOnly → blue CONNECTING）
- [x] install-time 初始 state = Connecting（避免 1-frame Active 闪导致 lost-image renderer 跳过）
- [x] image fallback 在 FallbackActive 时仍渲染（外部 cell + RetryWithFallback + image type）
- [x] external cell M5 fallback 链路接通（PGM/PRVW/Scene/Source/Image 都能在外部 cell 上工作）
- [x] external VU 与 trackMode 正交（外部 cell 总是 metering 自己的私有源；Spout silent）
- [x] HealthCode/SignalRuntimeState `Paused`：FFmpeg/VLC OBS_MEDIA_STATE_PAUSED 不再误报 Lost
- [x] 行内 pillarbox snap 阈值 8→16 px：消除整数除余 1px 在相邻 cell 上 snap 不一致
- [x] 所有 [perf] / [health] / [fill] 日志加 `[<inst-name>(<uuid8>)]` 前缀
- [x] CMakePresets 切到 Visual Studio 18 2026（本地工具链）

