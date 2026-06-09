# Phase 2 Hardening Notes

> 本文档记录 Phase 2（视觉参数系统：背景 / 标签 / 安全区 / VU meter / overlay）完成后做的代码硬化观察项与修复。
> 延续 [phase-1-hardening-notes.md](phase-1-hardening-notes.md) 的"已修复 / 观察项"结构。
> 修复范围：multiview-instance.cpp、multiview-window.cpp、config-manager.cpp、cell-display-settings-dialog.cpp。

---

## ConfigManager

### 已修复

- **`configVersion` 升级日志**：[config-manager.cpp](../src/config-manager.cpp) `load_from_file()` 现在区分三种情况：
  - 旧版本（`version < CURRENT`）→ 记录 `LOG_INFO` 升级日志；
  - 新版本（`version > CURRENT`）→ 记录 `LOG_WARNING`，提示部分字段可能被忽略；
  - 当前版本 → 无日志。
- **`CURRENT_CONFIG_VERSION` 升到 2**：v1 = Phase 1 baseline；v2 = Phase 2（新增 `visualSettings` 字段贯穿 global / instance / cell）。  
  Phase 1 配置缺失 `visualSettings` 时不会报错——各 `from_obs_data()` 都有完整的默认值回退路径，仅升级日志会出现。

### 观察项

- **解析失败仍只返回 false**：当前 `load_from_file()` 在 JSON 解析失败时返回 false 但不重置内存中的现有配置。Phase 3 如有更严格的容错需求，可考虑解析失败时主动重置为 safe defaults 并 backup 损坏文件。

---

## MultiviewInstance / VisualSettings

### 已修复

- **`LabelSettings::from_obs_data` 加上界**：[multiview-instance.cpp](../src/multiview-instance.cpp)
  - `fontSize`: clamp 到 `[1, 200]`（防止恶意配置导致 GDI+/text_ft2 渲染失败）
  - `minFontSize`: clamp 到 `[1, 200]`
  - `maxFontSize`: clamp 到 `[minFontSize, 400]`
  - `margin`: clamp 到 `[0, 200]`
  - `fontFamily`: 长度 clamp 到 128 字符（防止极长字符串拖慢 Qt 字体查找）
- **`VuMeterSettings` dB 区间合理化**：
  - `warningDB` / `errorDB`: clamp 到 `[-96.0, 0.0]`
  - 保证 `errorDB >= warningDB`（红区在 dB 轴上必然高于黄区，否则渲染逻辑会颠倒）
- **数据模型 = 持久化 = UI 三方一致**（来自上一轮设计文档审计）：
  - `fontFamily` 与 `backgroundRounded` 在 UI 端补齐；
  - 所有 5 个 settings group（Background / Label / SafeArea / VuMeter / Overlay）字段已逐一对照设计文档 5.0 节，全部存在于 UI。

### 观察项

- **`from_obs_data(nullptr)` 沉默**：所有 `*Settings::from_obs_data()` 在 `data == nullptr` 时返回默认对象但不记录日志。若 Phase 3 出现"配置看起来加载了但字段全空"的场景，可在 null 分支加 WARNING 帮助诊断。
- **Inheritance Mode 字段未做白名单校验**：`InheritanceMode` 从字符串解析时未知值会回退默认，但不报告原始字符串。可在 Phase 3 补充。

---

## MultiviewWindow

### 已修复

- **`bg_images_` / `overlay_images_` 缩容时的纹理泄漏**：[multiview-window.cpp](../src/multiview-window.cpp) `rebuild_bg_images()` / `rebuild_overlay_images()`
  - 此前只通过 `while (size < cellCount) push_back()` 增长，从未缩小；
  - 当 grid 从 10×10 改成 4×4 时，超出 cellCount 的旧纹理保留到窗口关闭才释放（实际占用显存）；
  - 修复：检测 `bg_images_.size() > cellCount` 时收集所有越界 entry 的纹理到 `textures_to_destroy`，然后 `resize(cellCount)`。释放仍走原有的"锁外 `obs_enter_graphics()`"路径，遵守锁顺序。
- **`release_volmeters()` 加锁保护 `cell_volmeters_`**：
  - 原实现不持有 `source_mutex_`，但渲染线程在 `render_vu_meter()` 中读取 `cell_volmeters_.size()` 与 `cell_volmeters_[i]`；
  - UI 线程通过 `refresh_sources()` → `release_source_refs()` → `release_volmeters()` 修改 vector 时，与渲染线程存在并发；
  - 修复：在 `release_volmeters()` 顶部加 `std::lock_guard<std::recursive_mutex> lock(source_mutex_)`。`rebuild_volmeters()` 本身已经先调用 `release_volmeters()` 再加锁，由于使用 `recursive_mutex`，递归加锁安全。

### 观察项

- **`refresh_visual_settings()` 在锁内调用 `rebuild_label_sources()`**：标签源创建/销毁是重操作（OBS 私有 source），锁内执行会延长 UI 线程对渲染线程的阻塞。后续可重构为"锁内收集数据 → 锁外创建/销毁 source → 锁内 swap"模式（参考 `rebuild_bg_images()` 的四阶段做法）。
- **`check_scene_change_for_volmeters()` 防抖**：从渲染线程每帧调用，当 PGM/PRVW 频繁切换时可能频繁触发 `rebuild_volmeters()`。当前未观测到性能问题，但 Phase 3 可考虑增加 100ms 节流。
- **`volmeter_callback` 写入 `SingleVolmeter` 字段无原子保护**：float 写入在 x64 上对齐时硬件原子，渲染线程读到的最坏只是"上一帧值"（VU meter 显示可容忍），不会撕裂。`last_callback_ns` (uint64_t) 同样依赖 64-bit 自然对齐。当前实现与 OBS 内置 `VolumeMeter` 行为一致，不打算改动。
- **窗口隐藏时 VU meter 仍在工作**：`obs_volmeter_*` 在窗口隐藏后继续接收音频回调（约 100 Hz × meter 数）。监视器场景下窗口通常常开，影响有限。后续可在 `hideEvent` 调用 `release_volmeters()`、`showEvent` 调用 `rebuild_volmeters()`。
- **上下文菜单 cellIndex 失效**：在菜单弹出与点击之间用户可能通过 Edit Grid 改变布局。Cell Display Settings 入口已通过"重新计算 layout 并验证 `cellIndex < cells.size()`"自我保护；Change Source / Clear Cell / Add Source 入口依赖现有 `on_change_source()` 内部检查。当前未出现问题，但可作为统一约定记录。

---

## CellDisplaySettingsDialog

### 已修复

- **OBS-native 选色器**：替换原有"裸 line edit 输 #RRGGBB"为 `QColorDialog` + 28×22 色块按钮（Background color / Label text color / Safe area color），与 OBS 内置 `obs_properties_add_color` 使用同一组件。
- **OBS-native 文件选择器**：图片路径字段配 "Browse..." 按钮，调用 `QFileDialog::getOpenFileName()`，过滤器默认 `All Files (*)`；保留直接路径输入能力（Background image / Overlay image）。
- **OBS-native 字体选择器**：新增 `btn_label_font_`，点击调用 `QFontDialog::getFont()`，所选字体名作为按钮的显示字体实时预览，与 OBS `text_gdiplus` 字体属性体验一致。
- **`backgroundRounded` UI 暴露**：Label 组新增 "BG Rounded" 复选框，与数据模型 / 持久化字段同步。

### 观察项

- **dirty flag 利用不充分**：`dirty_` 标志已存在，但对话框 `Accepted` 时无条件 `save()`。100 cell 场景下 UI 改一个字段就触发全 instance 的 JSON 重写。可在 `accept()` 中加入"初始 hash vs 当前 hash"对比。
- **lambda 捕获 `swatch` / `edit` 的生命周期**：使用 Qt context object (`QObject::connect(edit, signal, swatch, lambda)`) 模式，Qt 在 context object 析构时自动断开连接，无 UAF 风险。已确认安全，无需改动。

---

## CI / Build

### 观察项

- **Phase 2 仅在 RelWithDebInfo + Windows 验证**：macOS / Linux 在 Phase 2 中未做手动验证；CI workflow 存在但运行结果未确认。

---

## Documentation Hygiene

### 已修复

- **`docs/setup/deploy-plugin.ps1` 编码**：PS 脚本含中文，需保存为 UTF-8 with BOM 才能被 Windows PowerShell 5.x 正确解码（无 BOM 时 5.x 按 ANSI 解码导致 parser 失败）。修复后 `powershell` (PS 5.x) 与 `pwsh` (PS 7) 均可运行。
- **设计文档字段审计**：[phase-2-visual-settings-design.md](phase-2-visual-settings-design.md) 5.0 节中 5 组 settings 全部字段已逐一对照实现，UI / 数据模型 / 持久化三方一致。

---

## 修复清单（本次提交）

| 文件 | 修复 | 风险等级 |
|---|---|---|
| `src/multiview-instance.cpp` | LabelSettings 字体/边距上界 clamp | HIGH |
| `src/multiview-instance.cpp` | VuMeterSettings dB 区间 clamp + 顺序约束 | MED |
| `src/multiview-window.cpp` | bg_images_ / overlay_images_ 缩容时纹理泄漏 | MED |
| `src/multiview-window.cpp` | release_volmeters() 锁保护 cell_volmeters_ | HIGH |
| `src/config-manager.{hpp,cpp}` | CURRENT_CONFIG_VERSION 升到 2，加迁移日志 | LOW |

---

## 第二轮硬化（VU 动态更新 / Track Source UI 之后）

本轮覆盖 commit `821d623` 引入的代码：per-source 信号订阅（`mute` / `audio_mixers`）、
1 Hz 活跃源指针集合轮询、`compute_active_track_bit()`、PRVW Studio Mode OFF → PGM 回退、
Track Source / Manual Track UI 控件。

### MultiviewWindow（追加已修复）

- **`rebuild_volmeters()` per-source LOG_INFO 日志风暴**：[multiview-window.cpp](../src/multiview-window.cpp)
  - 原实现在 attach 循环内对每个源 `obs_log(LOG_INFO, "VU meter cell %d: attached '%s'", ...)`；
  - 与本轮新增的 1 Hz 活跃源轮询叠加后，任何场景树变更（重命名 / 加滤镜 / 切轨道）都会触发 rebuild，
    10×10 grid + 嵌套场景下单次 rebuild 可产生上百条日志，淹没 OBS 日志窗口；
  - 修复：循环内不再 LOG_INFO，rebuild 末尾输出 **单行 summary**：
    `VU meters rebuilt: cells=%d sources=%d track_bit=0x%x`。
- **`collect_audio_sources_cb` MAX_DEPTH 静默截断**：
  - 原实现达到 `MAX_DEPTH=8` 时直接 `return true`，无任何日志；
  - 病态嵌套场景下用户看到部分源缺失却找不到原因；
  - 修复：`AudioCollectCtx` 增加 `depth_exceeded` 一次性标志，由 `collect_audio_sources()`（顶层调度）在该 flag 为真时输出 **单条** `LOG_WARNING`，包含源名。"每次 collect 调用最多一条" 避免递归点放大成日志风暴。
- **`compute_active_track_bit()` 多余的 effective 解析**：
  - 原实现为读 `trackMode` / `manualTrackIndex` 调用了完整的 `resolve_effective_visual_settings()`（涉及结构体拷贝 + 继承 3 层 walk）；
  - 该函数在 1 Hz 轮询路径上调用，且 v1 中 trackMode 只在 instance 层有意义（不存在 per-cell 覆盖）；
  - 修复：直接读 `inst->visualSettings.vuMeter`，节省每秒一次的多余分配 / 拷贝。
- **`release_volmeters()` disconnect 路径的安全保证文档化**：
  - 通过 `OBSGetStrongRef` 取强引用做 disconnect；若源已被销毁返回 null，跳过 disconnect；
  - 此分支看起来"漏掉了" disconnect，实际上 OBS 源销毁时其 `signal_handler_t` 一起销毁，已注册的 handler 不可能再触发，**by construction 安全**；
  - 补充注释明确这一不变式，避免后续误以为是 bug。

### MultiviewWindow（追加观察项）

- **1 Hz 活跃源轮询在窗口隐藏时仍运行**：与已有的"窗口隐藏时 VU meter 仍在工作"观察项同源。`obs_display_*` 回调在窗口最小化 / 隐藏时仍会触发，轮询会继续遍历场景树。监视器场景下窗口常开，影响有限；如需修复可在 `hideEvent` 暂停轮询、`showEvent` 立即 rebuild。
- **`last_active_sources_` 指针集合比较的 freed-then-reallocated 边界**：
  - 比较的是 `void*`，理论上"源 A 销毁 → 同地址被源 B 复用"会被识别为"未变化"，跳过 rebuild；
  - 实际上场景树发生这种变化必然伴随 `obs_source_create` / `_destroy`，而 OBS 自身的 scene-item 信号会触发 `refresh_sources()` 走另一条 rebuild 路径；纯 1 Hz 比较只是兜底，极端 race 下最多延迟 1 秒；
  - 不修，记录在案。
- **`Track Source = Manual` + streaming output 未包含该 track**：用户主动选择，PGM cell 在 manual track 上可能没有任何源 → VU 全空。属正常行为（与 OBS 本身"切换到没人录的轨道"语义一致），不视为 bug。

### 修复清单（本轮提交）

| 文件 | 修复 | 风险等级 |
|---|---|---|
| `src/multiview-window.cpp` | rebuild_volmeters per-source LOG_INFO → 单行 summary | MED（可读性） |
| `src/multiview-window.cpp` | collect_audio_sources MAX_DEPTH 命中时 LOG_WARNING（一次性） | LOW（可诊断） |
| `src/multiview-window.cpp` | compute_active_track_bit 直接读 instance 设置，去掉 effective 解析 | LOW（性能） |
| `src/multiview-window.cpp` | release_volmeters disconnect 安全不变式补注释 | LOW（可维护性） |

### MultiviewWindow（追加已修复 · 日志可读性补丁）

- **VU 日志缺少 instance / cell 标识**：[multiview-window.cpp](../src/multiview-window.cpp)
  - 原 summary 仅打印 `cells=N sources=M track_bit=0xN`，多 instance / 几十 cell 场景下完全不可定位；
  - 修复（同次硬化的回归补丁）：
    - rebuild summary 加 `[name(uuid8)]` 前缀 + per-cell 简表 `c0:pgm=2 c2:scene=1 ...`；
    - `collect_audio_sources` 不再自带 `LOG_WARNING`，改为返回 `bool depth_exceeded`；rebuild 端聚合所有命中 cell 写一条 `[name(uuid8)] VU meter scene walk hit MAX_DEPTH=8 in cells [c2,c5] ...`；
    - 1Hz 轮询路径 `collect_active_source_pointers` 故意不打这条 warning（避免随轮询频率刷屏），由真正的 rebuild 路径承担。
  - 命名遵循 `[实例名(短UUID前8位)]` 约定，与未来其它子系统日志保持一致。

| 文件 | 修复 | 风险等级 |
|---|---|---|
| `src/multiview-window.cpp` | VU 日志加 instance + per-cell 标识；MAX_DEPTH 警告聚合 | LOW（可观测性） |

