# Issue #20 — 嵌套 AMV 源设计文档（AMV 实例作为 per-cell 源）

状态：设计定稿，实现中（P1 起）（测试后置：功能可本机测，先实现+主会话验收+对抗审查）
范围：新增 per-cell 源类型「一个 AMV 实例」——实例 A 的某个 cell 显示另一实例 B 的合成画面
关联：issue #11 外部输出层（Spout/NDI）、issue #16/#18（DeckLink/AJA 输出，同为「复用/新增 obs 对象」范式）、A1/A2 输出后端注册表、M5 内部源 weak-ref 渲染

> **本质区别（先读）**：这是**按需拉取**——A 在图形线程、进程内直接采样 B 的合成 GPU 纹理。
> **不是外部输出**：外部输出是**推**（CPU 回读 + 真的 OBS 输出对象 + 硬件/网络，持续吐像素）。
> 两者只在「选分辨率」的 UI 词汇上相似，数据通路毫无关系。

---

## 0. 最高原则（先过这四条）

1. **绝不炸 OBS**：跨实例合成若走「A 绘制时同步触发 B 合成」，遇到环（A→B→A、自指、深链）会在同一图形线程无限重入 `draw_cells` → 爆栈崩 OBS。本设计用「**只读 B 已发布的上一帧**」根除这条。
2. **非阻塞**：绝不在 A 的绘制里同步跑 B 的合成或阻塞等待 B；只采样一块已完成的纹理句柄，不取 B 的锁。
3. **非入侵 / 隔离**：目标实例缺失/无帧只降级为该 cell 走 lost 路径，不连累 A 的其它 cell、其它实例或 OBS 本体。
4. **广播级稳定**：反复增删引用、缩放窗口、切场景集、删除/改名目标都不崩不卡；跨实例纹理生命周期与 core 生命周期严格协调。

### 与外部输出的根本差异（务必先读）

| 维度 | 外部输出（Spout/NDI/DeckLink/AJA） | 嵌套 AMV 源（本设计） |
|---|---|---|
| 方向 | 推：把合成帧吐出去 | 拉：把 B 的合成帧读进来 |
| 通路 | GPU 回读到 CPU + 真 `obs_output` 对象 + 硬件/网络 | 进程内、图形线程直接采样 B 的 GPU 纹理，无回读、无输出对象、无网络 |
| 触发 | 每帧持续 | A 绘制该 cell 时采样 B 的已发布纹理 |
| 复用的东西 | —— | 仅复用「选分辨率」的 `OutputResolutionMode` 预设 UI（手动档） |

## 1. 承重现状（均已对源码核实）

- **本插件今天注册零个 obs_source**。每个外部 cell 都是 `ISignalProvider::create_private_source` 返回的 `obs_source_t`，runtime 每帧 `inc_active` + `obs_source_video_render`（`signal-provider.hpp:109`；`amv-instance-core-sources.cpp` 的 refresh_cell）。→ 注册一个**隐藏** obs_source 即可完全复用这条轨道。
- **没有任何实例保留自己的合成纹理**。`draw_cells` 直接画进当前渲染目标；唯一「合成→纹理」是输出通道（`render_output_only` → `MultiviewOutputManager::render_all` 的每帧 texrender，reset/复用、不暴露——`amv-instance-core-sources.cpp:1177`；`multiview-output.cpp`）。→ 需新增发布用缓冲。
- **core 惰性**：`g_cores`（`plugin-main.cpp:76`，文件静态、uuid 键、`g_registry_mutex` 守护）仅在实例有开着的窗口或启用输出时存在（`ensure_core:81`；拆解 `:231`/`:250`）。被引用的 B 若两者都无 → 无 core、从不合成。
- **递归是真实崩溃点**：`draw_cells` 持 per-instance `std::recursive_mutex`（`amv-instance-core-draw.cpp:62`）；环上的同步嵌套合成会在同线程重入（不是死锁）→ 递归到爆栈。OBS 靠 **add-time DAG** 防场景递归（`obs_source_add_active_child`，`libobs/obs-source.c:4732`：拒 parent==child + 遍历子树），**非渲染期**。发布帧模型整类绕开。
- **UI 随合成像素缩放**：scene 标签目标高 ≈ `cell.h / 14.7`，夹到 `[minFontSize, maxFontSize]`（`amv-instance-core-label.cpp:232`）；Fixed 模式用绝对像素。→ 合成分辨率必须做成 per-cell 可选。
- `OBS_SOURCE_CAP_DISABLED`（`libobs/obs-source.h:169`）：注册后从「添加源」列表隐藏，但仍可 `obs_source_create[_private]` 创建（OBS 自身 `obs.c:1219` 即这么用）。
- **身份**：实例 uuid 稳定且持久化；name 可改（`rename_instance`）、clone/#14 重复会重名。引用**只用 uuid**。
- **API 语义以构建依赖那份为准**（AGENTS §2）：本文引用的 libobs 行为读自源码树 `obs-studio`；实现时凡涉及 `obs_source_info` 注册、`OBS_SOURCE_CAP_DISABLED`、`create_private_source` 契约，以 `.deps/obs-studio-31.1.1` 的 `obs.hpp`/头文件语义为准复核（源码树版本可能不同）。

## 2. 已锁定决策（用户逐条定）

1. **机制**：注册真 obs_source `amv_instance_source`，加 `OBS_SOURCE_CAP_DISABLED`（OBS「添加源」列表里不可见、仍可程序内创建）。仅从 AMV 自己的 picker 选择；OBS 内直接添加为后续工作（将来去掉 flag 即开放）。
2. **拉取模型**：消费者读 B **已发布的上一完成帧**，绝不同步触发 B 合成。B 独立、每帧被驱动合成。→ 自指、2-环、任意深链**按构造安全**——**不做**环检测、**不做**渲染深度上限。代价：每跳 ≤1 帧延迟。
3. **分辨率（per-cell 可选）**：
   1. 跟随屏幕——B 按 A 窗口所在屏的分辨率合成；
   2. **跟随窗口（默认）**——B 按该 cell 的实际像素合成、封顶屏幕分辨率；最忠实「UI 随实际显示尺寸缩放」的既有体验、最锐利；
   3. 手动——复用外部输出预设（画布 / OBS 输出 / 推流重缩放 / 录像重缩放 / 自定义，即 `OutputResolutionMode` + `resolve_output_dimensions`）。
4. **画面**：完整 B（标签/VU/高亮/叠加）或仅网格；用户选，默认完整。
5. **音频**：v1 不做跨实例计量。可见的电平仅是完整模式下 B 画面里自带的 VU 像素。picker/editor 给一个**禁用 + 未勾**的占位 checkbox，hover 文案「待评估」。（"B 的音频"无单一定义——B 是多源监看、有自己的 per-cell 路由/VU——真正的 A 侧计量待明确后再做。）
6. **保活**：per-cell 开关「无窗也让目标运行」，默认开（用户可能永不单开 B 的窗）。
7. **失效目标**（B 删除/改名/切场景集）：复用现有 signal-lost 路径；引用按 uuid，改名透明。
8. **自指**：容忍（发布帧模型下是安全反馈）。picker 把当前实例行标为斜体 + 加粗 + 「当前实例」后缀，不禁止选择。

## 3. 架构

### 3.1 B 发布自己的合成帧
给 `AmvInstanceCore` 增加「消费者合成」服务：一个以 `(分辨率 R, 画面模式)` 为键的 map → **双缓冲** texrender，按活跃引用方引用计数。每帧由驱动对每个被请求的键把 `draw_cells`（完整或仅网格）画进 back，帧末交换为 front。消费者采样 front。双缓冲保证消费者永不读到写一半的缓冲——包括自指（A 读自己上一帧 front，同时往 back 合成）。

### 3.2 保活（core 创建/销毁只在 UI 线程 reconcile，绝不图形线程惰性触发）
给 core 增加第三个存活理由：「被 ≥1 个活跃 cell 引用为拉取源」。引用计数归零即释放（仿现成的无窗输出宿主 `reconcile_output_host`）。新增 by-uuid 的 find/ensure/ref/unref-core API（声明在 `multiview-window.hpp`，由 `g_cores` 在 `g_registry_mutex` 下支撑）。被引用且保活开的 B 即使无窗/无输出也强制建 core 并每帧驱动（tick + 消费者合成）。

**关键线程归属（AGENTS §2 硬规则）**：`ensure_core` 会做重活（`refresh_layout` / `apply_output_settings`，遍历场景树、可能触发 `inc/dec_active`/第三方回调）——**绝不能在图形线程或持 `source_mutex_` 时做**。因此：
- **引用计数与 core 创建/销毁只在 UI 线程 reconcile**：由「cell 赋值变更」这个 UI 线程事件触发（新增一个 `reconcile_pull_hosts()`，与 `reconcile_output_host` 同层同线程），扫描所有实例的 cellAssignments，算出「被引用集合」，据此 ensure/destroy 目标 core。**绝不从 A 的图形线程绘制里惰性创建 B 的 core。**
- **图形线程只做两件事**：驱动已存在的 core 每帧把消费者合成画进发布缓冲；A 的源采样 B 的 front 缓冲。若引用刚建、reconcile 尚未跑（B 的 core 还没起），该 cell 先走 Lost 一两帧，等 UI 线程 reconcile 建好 B 即恢复。
- **销毁走四阶段**（AGENTS §2）：锁内收集待销毁 core → 释放锁 → 先从渲染驱动摘除（图形锁下，确保无在途帧持有）→ 再销毁（在 UI 线程释放其源）。沿用 `on_window_closed`/`reconcile_output_host` 现成纪律。

### 3.3 隐藏源 `amv_instance_source`
带 `OBS_SOURCE_CAP_DISABLED` 注册。设置：目标 uuid、分辨率模式+参数、画面模式、保活开关。其 `video_render`：
- **每帧按 uuid 重解析 B 的 core**（绝不跨帧缓存 core 指针或纹理句柄——B 可能被拆）。**解析走图形锁下维护的 `uuid→core*` 快照，绝不取 `g_registry_mutex`**：`video_render` 由 OBS 在持图形锁时调用，若再取 `g_registry_mutex` 就是 `graphics→registry`，与所有 UI 线程路径的 `registry→graphics`（如 `multiview_refresh_output_driver`）AB-BA 倒置、会死锁。P1 已提供的 `multiview_find_core` 取的是 `g_registry_mutex`，故**只能 UI 线程用（枚举/校验），采样方绝不能用**。快照做法：仿 P1 的 `g_consumer_hosts`，在 `multiview_refresh_output_driver`（图形锁段内）额外维护一份 `uuid→core*`（限被拉取的 core），采样方在图形锁下无锁读——P2 落地此结构。
- 为解析出的 `(R, 画面模式)` 调 `note_consumer_demand` + 采样 `get_consumer_front` 的 **front** 缓冲并绘制（默认 effect，同 bg 图 blit `amv-instance-core-draw.cpp:815`）；
- B 缺失或尚无帧 → 不绘制 → 该 cell 经现有健康/Lost 路径降级。

其 `get_width/get_height` 报告 R，使 `draw_cells` 的 letterbox 计算不变。`create_private_source` 返回 `obs_source_create_private("amv_instance_source", …)`；`refresh_cell`、健康监督、`draw_cells` 都把它当作普通外部源，**不改**。

### 3.4 分辨率模式 → R
**R 是「窗口/屏幕级」的大尺寸，绝不是 cell 尺寸**：cell 在网格里很小，若按 cell 像素合成 B，B 的 UI（标签/VU 有最小字号钳制）相对那一小块会巨大变形。正确是让 B 在窗口/屏幕级尺寸下合成（看起来就像在该尺寸窗口里），再整幅缩小进小 cell。
- 手动：`R` 取预设（`resolve_output_dimensions`：画布/输出/重缩放/自定义）。固定、可共享。
- 跟随屏幕：`R` = 拉取方窗口所在屏的分辨率（Qt/UI 概念，需从 MultiviewWindow plumb 到源；不随窗口内缩放变）。
- 跟随窗口（默认）：`R` = 正在渲染该 B-cell 的 **MultiviewWindow 实际渲染目标（整窗）尺寸，含被拉伸后的真实宽高比**（随窗口缩放变化，封顶屏幕分辨率）。取值走 `video_render` 里的当前渲染目标尺寸（核实 `gs_getsize()` 返回的是窗口/display 而非 cell 视口）——**不是** cell 矩形/`gs_get_viewport`，也**不是**把 R 套成画布宽高比的 letterbox 盒子。窗口被拉成超宽/超窄，B 就在该真实尺寸+真实宽高比下用 consumer_engine_ 布局（B 的网格随之拉伸/压扁，= B 自己那个尺寸/形状窗口里的样子）。`get_width/get_height` 同报这个真实 R（不套画布比），letterbox 数学与 R 一致、绝不二次缩放/黑边填充。要点：**窗口级大尺寸（非小 cell，避免 UI 巨大）＋真实宽高比（不 letterbox）两者同时满足**。A 可有多窗口（issue #10），同一 B-cell 在不同尺寸窗口按各自 R 合成（消费者合成按 (R, 画面模式) 键支持多 R，封顶 kMaxConsumerTargets=8）；拖动窗口时对 R 做量化/防抖避免合成抖动。

### 3.5 递归 / 时序安全
消费者只读已发布 front；B 的合成由驱动独立完成。无同步嵌套合成 ⇒ 任何环/自指/深度都不递归 ⇒ 无需环检测、无需深度上限。跨实例渲染顺序未定，故消费者会看到 B 的上一帧（当 B 在其后合成）：每跳 ≤1 帧延迟、沿链累加——**文档化，非缺陷**。读已完成纹理句柄不取 B 锁；两个 core 的 `source_mutex_` 绝不嵌套。

### 3.6 身份与持久化
在源的 `obs_data` 设置里存目标 **uuid**（+ 分辨率模式/参数、画面模式、保活），经现有外部 cell 的 `SignalConfig` 路径持久化。不认识该类型的构建把它解析到 `Unknown` 前向兼容槽并原样保留 payload。

## 4. UI 触点
- `SignalProviderType` 枚举值 + 稳定字符串 id `"amv_instance"`；provider 类 + 注册钩子；`make_provider_settings_form` 分支；picker 标签页；editor 抬头；i18n 键**两个** locale（`en-US.ini` + `zh-CN.ini`）；`CMakeLists.txt` 源列表。
- `SourcePicker` 构造函数需增加 `ConfigManager` + 当前实例 uuid（现为 parent-only），以便枚举实例、存所选 uuid、标记当前实例行（斜体+加粗+「当前实例」，可选）。
- 设置表单：实例选择器、三档分辨率选择器（手动档复用外部输出预设）、完整/网格画面开关、禁用的「音频」占位 checkbox（hover「待评估」）。

## 5. 已知限制 / 风险（v1）
- 每跳 ≤1 帧延迟；深链累加。监看场景可接受。
- 跟随窗口档在 cell 尺寸变化时重合成、且不同尺寸的消费者不能共享 → 成本 = 每个被引用实例每帧的不同 `(R, 画面模式)` 键数（通常 1–2）。
- 跨实例音频计量不做（ill-defined），仅占位。
- 被引用且保活开的 B 会在后台无窗运行（资源成本），直到引用归零。
- 源已注册但隐藏；OBS 内直接使用为后续工作。

## 6. 分阶段（实现下放 opus；每阶段主会话逐行验收 + 对抗审查）
- **P1 运行时骨干**：消费者合成服务（双缓冲、按 `(R, 画面模式)` 键、引用计数）+ 保活/ref-core API + 驱动接入。
- **P2 隐藏源**：注册 `amv_instance_source`（CAP_DISABLED）、`create_private_source` 接线、缺失目标走 Lost。
- **P3 UI**：provider 类型 / picker 标签 / 设置表单 / editor 抬头 / 序列化（uuid）/ i18n / CMake；三档分辨率 + 预设；自指标识；音频占位。
- **P4 硬化**：跟随窗口防抖/量化、拆解竞态、每实例合成成本封顶、延迟文档化、删除/改名/切场景集的缺失目标处理。

## 7. 测试清单（真机使用，部分后置）
- A 显示 B；缩放 A → B 跟随（档 2）；切换档 1/3；A 在不同屏。
- 完整 vs 仅网格。
- 自指（A 显示 A）：反馈画面、不崩；当前实例行有标识。
- 2-环（A↔B）、3-环、深链：不崩、延迟有界、画面正确。
- 删除/改名 B、切场景集：cell 走 Lost、不崩。
- 保活开/关且 B 窗口关闭。
- B 被多个实例/多种尺寸引用时的成本。
- 与外部输出及所有既有 cell 类型并存；无回归。
