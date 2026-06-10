# Phase 2 验收清单

> 本文档用于把 Phase 2（M4：视觉参数与辅助功能）已完成内容变成可复测、可交接、可回归的事实。  
> 术语口径以 [docs/TERMINOLOGY.md](docs/TERMINOLOGY.md) 为准；Phase 2 = M4，本文档中 “M4 子任务 2.0~2.7” 对应 [docs/phase-2-visual-settings-design.md](docs/phase-2-visual-settings-design.md) 第 16 节的拆分。  
> 进入 Phase 3（M5~M6）之前，应先确认本清单中的关键项均已验证，未确认项需要在 Phase 2.5（M4 收尾 / Phase 3 准备）期间补齐或显式标注为非阻塞。

图例：

- `[x]` — 代码已实现 + 已人工验证。
- `[o]` — 代码已实现，仅在 Windows RelWithDebInfo 单机验证，跨平台 / 跨配置未确认。
- `[ ]` — 代码未实现，或属于 Phase 2.5 后续任务，或显式跨阶段（M5+）。
- `~~[ ]~~ **已移除/不做**` — 设计决策明确不实现，仅供历史追溯。

---

## 2.0 Visual Settings 架构与继承模型（M4 子任务 2.0）

- [x] `GlobalVisualSettings` 结构定义于 [src/multiview-instance.hpp](src/multiview-instance.hpp)
- [x] `InstanceVisualSettings` 结构定义，包含每组 `InheritanceMode`
- [x] `CellVisualSettings` 结构定义，包含每组 `InheritanceMode`
- [x] `EffectiveCellVisualSettings` 解析结果结构定义
- [x] 分组级继承：Background / Label / SafeArea / VuMeter / Overlay 各自独立 `InheritanceMode`
- [x] `resolve_effective_visual_settings()` 实现继承链 cell → instance → global → 默认值
- [x] 旧 Phase 1 配置（无 `visualSettings`）正常加载，各 `from_obs_data()` 走默认值回退
- [x] `CURRENT_CONFIG_VERSION` 升到 v2，加载时记录升级日志
- [x] auto-save：修改视觉参数后立即写盘
- [x] 不破坏 Phase 1 行为：布局编辑、cell assignment、source 生命周期保持不变
- [x] Highlight（PGM/PRVW 边框）作为第 6 组接入，scope 只到 Global/Instance（设计决策，per-cell 不参与）

## 2.1 Label Settings（M4 子任务 2.1）

- [x] `LabelDisplayMode` 三种模式：`None / Overlay / Below`
- [x] `LabelPosition` 两个位置：`Top / Bottom`（Phase 2.5 不扩展 Left/Right/Center）
- [x] `FontScaleMode`：`Fixed / ScaleWithCell`
- [x] `minFontSize` / `maxFontSize` 边界 clamp 落实在 `LabelSettings::from_obs_data()`
- [x] `fontFamily` 长度上限 clamp 防御
- [x] `textColor`、`backgroundOpacity`、`backgroundRounded`、`margin` UI 暴露
- [x] OBS-native `QFontDialog` 字体选择器
- [x] OBS-native `QColorDialog` 文本色选择器
- [o] `Below` 模式视频区域正确缩小，不遮挡画面（Windows 单机验证）
- [o] `ScaleWithCell` 在 10x10 + span 下不超出 cell（Windows 单机验证）
- [o] 重启 OBS 后所有 Label 字段恢复

## 2.2 Background Settings（M4 子任务 2.2）

- [x] `BackgroundSettings` 字段：`colorEnabled / color / imageEnabled / imagePath / imageFitMode / fillMode / labelRegionFill`
- [x] `ImageFitMode`：`Fit / Stretch`（Phase 2.5 不扩展 Fill/Tile）
- [x] 底色支持透明源（Alpha）下显示
- [x] 底图加载走 `obs_enter_graphics()` 锁外创建，遵守锁顺序
- [x] 缩小网格时旧纹理被释放（[src/multiview-window.cpp](src/multiview-window.cpp) `rebuild_bg_images()` 修复缩容泄漏）
- [x] 图片文件不存在时优雅回退（不崩溃，不阻塞渲染）
- [o] 透明 Scene / Source 与底色叠加结果符合预期
- [o] 空 cell 底色显示正确
- [o] 重启 OBS 后底色 / 底图字段恢复
- [ ] 图片资源管理 / 素材库 / 复制到 `plugin_config` — **不做**（Phase 2.5 决策）

## 2.3 Overlay 坐标空间与前景 Overlay（M4 子任务 2.3）

- [x] `CellRect` 与 `SignalRect` 在 [src/multiview-window.cpp](src/multiview-window.cpp) `render()` 中显式区分
- [x] `OverlaySettings` 字段：`enabled / imagePath / opacity / fitMode / anchorMode`
- [x] `OverlayAnchorMode`：`Cell / Signal`
- [x] `OverlayFitMode`：`Fit / Stretch`
- [x] Overlay 图片在 letterbox / pillarbox 下按 anchor 正确定位
- [x] Overlay 纹理释放与 `rebuild_overlay_images()` 缩容同 bg 一致
- [o] 自定义透明 PNG 叠加在视频画面上方表现正确
- [o] span cell 下 Overlay 行为正确
- [ ] 多 overlay 图层栈 / overlay preset 管理 — **不做**（Phase 2.5 决策）
- [ ] 未来内置 safe-zone 图示通过插件 DLL 旁资源文件夹动态读取，不进入 Phase 2 主体

## 2.4 Safe Area（M4 子任务 2.4）

- [x] `SafeAreaSettings` 字段：`enabled / preset / color / opacity`
- [x] `SafeAreaPreset`：`EBU_R95`（Phase 2.5 不扩展其它 preset）
- [x] 安全区基于 `SignalRect` 渲染，不画到 letterbox / pillarbox 黑边
- [x] 使用 OBS 原生风格 `gs_vertbuffer_t` + `GS_LINESTRIP` 实现（[src/multiview-window.cpp](src/multiview-window.cpp) `init_safe_area_vbs()` / `render_safe_area()`）
- [x] vertex buffer 与 cell 尺寸同步重建
- [o] 重启 OBS 后安全区开关与颜色字段恢复
- [ ] SMPTE RP 218 / Action Safe / Title Safe 多 preset — **不做**，未来作为 overlay 资源提供

## 2.5 VU Meter（M4 子任务 2.5）

### 已实现

- [x] `VuMeterSettings` 字段：`enabled / position / opacity / width / style / anchor / flip / lengthRatio / warningDB / errorDB / decayRate / alignment / trackMode / manualTrackIndex`
- [x] 四向位置：`Left / Right / Top / Bottom`
- [x] `VuMeterAnchorMode`：`Cell / Signal`
- [x] `VuMeterDecayRate`：`Fast (23.5 dB/s) / Medium (11.76 dB/s) / Slow (8.57 dB/s)`
- [x] `VuMeterAlignment`：`Start / Center`
- [x] `VuMeterTrackMode`：`AutoFollowStreaming / Manual`（窗口级，per-cell deferred）
- [x] `warningDB` / `errorDB` 区间 clamp + `errorDB >= warningDB` 顺序约束
- [x] 三色段渲染（绿 / 黄 / 红）按 dB 阈值划分
- [x] `obs_volmeter_*` 接入；callback 写入 `SingleVolmeter` 安全
- [x] `release_volmeters()` 在 `source_mutex_` 保护下操作 `cell_volmeters_`
- [x] 缩容时旧 volmeter 正确 detach + destroy
- [x] PGM/PRVW cell + Studio Mode OFF 时 PRVW fallback 到 PGM
- [x] `compute_active_track_bit()` 1Hz 轮询 + scene-change 触发 rebuild
- [x] `collect_audio_sources()` 防环 + MAX_DEPTH 防御
- [x] rebuild summary 单行日志，避免 per-source LOG_INFO 风暴
- [x] mute 状态 WYSIWYG：UI mute 时贡献为零

### Phase 2.5 polish（设计待定，实现未开始）

- [ ] Peak Hold（含 `peakHoldMs`）
- [ ] dB 标尺 / 刻度显示（默认 `-60 / -40 / -20 / -9 / 0`）
- [ ] Scene cell trackMode 语义决策（默认继承 instance）
- [ ] Source cell trackMode 语义决策（per-cell manual track 是否实现）
- [ ] PGM/PRVW cell trackMode 文档定案（沿用 AutoFollowStreaming）

### 明确不做

- [ ] 完整复制 OBS Mixer 行为 — **不做**
- [ ] 5.1 / 7.1 多声道独立 meter — **低优先级**，Phase 2.5 不做
- [ ] 外部信号（NDI/Spout/媒体流）音频 meter — 属于 Phase 3（M6）

## 2.6 Visual Settings UI 整合（M4 子任务 2.6）

- [x] Settings tab 暴露 Global Visual Settings 入口（[src/manager-dialog.cpp](src/manager-dialog.cpp) `Edit Global Visual Settings...`）
- [x] Instance 详情页暴露 Instance Visual Settings 入口（`Instance Visual Settings...`）
- [x] `CellDisplaySettingsDialog` 三种模式：`Global / Instance / Cell`
- [x] 6 个 group：Background / Label / Safe Area / VU Meter / Overlay / Highlight
- [x] 非 Global 模式下每组有 `Inheritance` 下拉（除 Highlight）
- [x] Highlight 在 Cell 模式下显示 "Highlight is instance-level" 静态提示并禁用整组
- [x] MultiviewWindow 右键菜单包含 "Cell Display Settings..." 入口
- [x] 文件选择器使用 OBS-native `QFileDialog`
- [x] 字体选择器使用 OBS-native `QFontDialog`
- [x] 颜色选择器使用 OBS-native `QColorDialog`
- [o] `dirty_` flag 已存在，但 accept 时无条件 save（hardening 观察项，Phase 2.5 不强制优化）
- [ ] 字段级 inheritance（每个字段独立继承）— 不做，Phase 2 决策为分组级继承

## 2.7 动态生效与持久化（M4 子任务 2.7 - 部分）

- [x] `MultiviewWindow::refresh_visual_settings()` 实现继承解析 + label source / bg / overlay / VU 重建
- [x] `plugin-main.cpp::notify_multiview_visual_settings_changed()` 广播到所有打开窗口
- [x] Global Visual Settings 修改后所有窗口动态刷新
- [x] Instance Visual Settings 修改后对应窗口动态刷新
- [x] Cell Visual Settings 修改后对应窗口动态刷新
- [x] 修改后 auto-save 写入 `settings-<场景集合名>.json`
- [x] 重启 OBS 后视觉参数完整恢复（Windows + RelWithDebInfo 单机）
- [x] `refresh_visual_settings()` 与渲染线程的锁顺序：image rebuild 在 `source_mutex_` 锁外，避免 ABBA 死锁
- [x] 缩容（grid 缩小）时旧 bg/overlay/volmeter 资源被释放
- [o] `refresh_visual_settings()` 锁内 rebuild_label_sources 是观察项，未阻塞用户

## Highlight（PGM/PRVW 边框，Phase 2 后期引入）

- [x] `HighlightSettings` 字段：`enabled / pgmColor / prvwColor / nestedDashed / dashLengthPx / dashGapPx / minThicknessPx`
- [x] 4 种高亮状态 + 优先级：`PgmDirect > PrvwDirect > PgmNested > PrvwNested`
- [x] 嵌套检测使用 `obs_source_enum_active_tree`（自带防环）
- [x] Studio Mode OFF 时 PRVW 自动归零，PRVW cell fallback 到 PGM 同时叠加黄底栏
- [x] dashed 边框拐角 t×t 实心方块 + 边间走 dash（无错位）
- [x] PGM 在 PRVW 之上绘制，避免邻接 cell 边框相互覆盖
- [x] `gutter == 0` 时使用 cell 内侧 `minThicknessPx` 描边
- [x] 小 cell 几何 guard：`outerW < 2*t || outerH < 2*t` 早退
- [x] 仅 Global/Instance scope，per-cell 在对话框中强制禁用整组
- [x] PGM/PRVW 主屏 cell 不渲染冗余高亮（`compute_cell_highlight` 早退）

## 跨平台 / 跨版本验证（Phase 2.5 与 Phase 4 共同覆盖）

- [x] Windows + Debug 构建成功
- [x] Windows + RelWithDebInfo 构建成功
- [x] Windows + Release 构建成功
- [o] OBS 31.1.1 上 Phase 2 功能可用
- [o] OBS 32.1 上 Phase 2 功能可用
- [ ] OBS 32.0 上 Phase 2 功能验证
- [ ] macOS 构建运行时验证（仅 CI 配置存在）
- [ ] Linux 构建运行时验证（仅 CI 配置存在）
- [ ] tag 触发的 GitHub Release artifact 构建验证

## 性能与回归（Phase 2.5 触及，完整验收在 Phase 4 / M8）

- [o] 4x4 + 全功能（Label + VU + Highlight）单机帧率可接受
- [ ] 10x10 + 全功能多窗口性能基线（属于 M8）
- [ ] 6x6+ 场景的 CPU / GPU 占用量化（属于 M8）
- [ ] 窗口隐藏 / 最小化时 VU + 1Hz 轮询的优化策略（hardening 观察项）

## 文档同步状态（Phase 2.5 Step 1~5 覆盖）

- [x] [docs/TERMINOLOGY.md](docs/TERMINOLOGY.md) 已建立
- [x] [plan.md](plan.md) M4 状态更新为 "主体已完成"
- [x] [README.md](README.md) 增加 Phase 1 / Phase 2 文档导航
- [x] [docs/phase-2-acceptance-checklist.md](docs/phase-2-acceptance-checklist.md) 本文件
- [ ] [docs/known-limitations.md](docs/known-limitations.md) 清理已过时条目（Phase 2.5 Step 4）
- [ ] [docs/phase-2-hardening-notes.md](docs/phase-2-hardening-notes.md) 与 [docs/phase-2-visual-settings-design.md](docs/phase-2-visual-settings-design.md) 引用本验收清单
- [ ] VU meter polish 设计文档（Phase 2.5 Step 7）
