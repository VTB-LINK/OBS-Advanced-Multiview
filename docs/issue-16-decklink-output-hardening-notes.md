# Issue #16 DeckLink 输出 — 发版前硬化审计笔记（AGENTS.md §5）

审计方式：5 路 read-only 对抗 agent（并发/引用计数·锁序、生命周期/拆除/退出、数据模型/序列化/迁移、状态机/时序、libobs API 契约）并行独立视角 + 综合去重保守复评。每条承重结论已对照 `OBS-Advanced-Multiview` 与 `obs-studio` 源码验证。

判级：HIGH = 可触发、会崩溃/挂起/阻塞 OBS 渲染线程或内存损坏（发版阻断）；MED = 矛盾态/数据丢失/自愈型缺陷；LOW = 理论路径/已分析接受。

**总评：初版存在 2 个发版阻断 HIGH，不可按现状发布。** 本轮修复 H1、H2（必修）+ M1、M2、M3（廉价且在播出信号上可见，一并修）；LOW 3 项记录接受。

**修复后状态（经二次对抗审查，见 §D）：H1 / H2 / M1 / M3 = RESOLVED；M2 = PARTIAL（仅覆盖显式/启动失败 stop，静默拔线不自愈，非回归）；无遗留 HIGH；新引入 3 个 LOW（均非阻塞）。代码完整、可发布，待真机回归测试。**

---

## 处置一览

| 编号 | 级别 | 摘要 | 处置 |
|---|---|---|---|
| H1 | HIGH | 主线程拆除在持 `obs_enter_graphics` 时内联跑阻塞式 `obs_output_stop/release`，卡渲染线程 | **本轮修** |
| H2 | HIGH | 未校验 `mode_id`(含默认0) 传入 `obs_output_create`，OBS 内部空指针崩溃 | **本轮修** |
| M1 | MED | 合成尺寸取自持久化快照而非运行时栅格，发散即逐帧静默丢弃、不自愈 | **本轮修** |
| M2 | MED | 无存活性检查，底层 output 死亡后仍报 Running、不自愈 | **本轮修** |
| M3 | MED | `video_t` colorspace 硬编码 709，HDR 模式错色 + 逐帧转换 | **本轮修** |
| L1 | LOW | `deck*` 字段对 Spout/NDI 子对象也读写（共享结构体死数据） | 接受（设计 §6 已明示取舍） |
| L2 | LOW | UI 任务交接在进程退出时可能不排空 → 良性泄漏/OS 回收 | 接受 |
| L3 | LOW | 非 Qt/headless 前端下 Starting 无超时逃逸 | 接受（OBS GUI 合成前已装 handler）；可选加超时 |

---

## A. 发版阻断（HIGH）

### H1 — 拆除在 graphics 锁内内联执行阻塞式 output 释放，卡渲染线程
命中：并发、生命周期（两镜头独立命中同根因）。
- 位置：`src/multiview-output-decklink.cpp:414`（`stop()` 的 `obs_queue_task(OBS_TASK_UI,&close_task,…,false)`）；上游 `src/amv-instance-core-sources.cpp:1224-1233`（`apply_output_settings` 的 `obs_enter_graphics→teardown_locked→obs_leave_graphics`）、`src/multiview-output.cpp:296`（`shutdown_graphics`）。
- CONFIRMED 链：`obs_queue_task(OBS_TASK_UI)` 调 `obs->ui_task_handler`（`obs.c:3318`），handler 用 `QMetaObject::invokeMethod(App(),…,Qt::AutoConnection)`（`OBSApp.cpp:1222`）；调用方在主线程时 AutoConnection→DirectConnection→`close_task` **内联**执行，仍在 `obs_enter_graphics` 括号内 → `obs_output_stop/release`→`obs_output_destroy`（`os_event_wait`+`pthread_join`+DeckLink `Deactivate`，数~十 ms）全程持图形锁 → 渲染线程阻塞 → 主节目/预览掉帧。经 `render_all`/reconcile（图形线程）的禁用路径是跨线程 QueuedConnection、异步安全；仅主线程发起的拆除（对话框禁用、`shutdown_graphics`、退出）命中内联。
- 修复：重 output 拆除（`obs_output_stop/release`、`video_output_close`、`audio_output_close`）绝不能在持图形锁/内联路径上跑。改用**恒异步投递**——`QTimer::singleShot(0, qApp, [raw]{ close_output_instance(...); })`（QTimer 单发永远入队、即使同线程也不内联），使拆除在当前栈（含 `obs_leave_graphics`）展开后于主线程运行；图形资源（`destroy_stages`）仍就地在图形线程释放。（本插件已惯用 `QTimer::singleShot(0,…)` 跨线程投递，见 `amv-instance-core-source-signals.cpp:219`。）create 投递仅从图形线程（configure_audio）发起、本就是 QueuedConnection 异步，不受影响。

### H2 — 未校验 mode_id 传入 obs_output_create → OBS 内部空指针崩溃
命中：libobs 契约（两站点）+ 数据模型（持久化注入向量）。
- 位置：后端 `src/multiview-output-decklink.cpp:193→197`（`configure_audio` 仅以 deviceHash 非空为门，`:349`，从不校验 modeId）；对话框探测 `src/external-output-settings-dialog.cpp:366→381-384`（空组合框时 modeId=0 仍探测）。
- CONFIRMED 链：`decklink_output_create`（`obs-studio/plugins/decklink/decklink-output.cpp:32-36`）`if(device)` 内 `FindOutputMode(modeID)` = `return outputModeIdMap[id];`（`decklink-device.cpp:201`，`std::map::operator[]` 未知键默认插入 `nullptr`）→ 紧接 `to.width = mode->GetWidth();`（`:36` 无空检查）→ 空指针解引用、进程崩溃。插件侧 `obs_output_get_video_conversion` 的 NULL 保护在 `obs_output_create` 返回之后，太晚。触发：设备**在场** + modeId 不在其模式表（0 = 组合框空时常态；或脏/跨机配置）。
- 修复：modeId 作为硬前置条件。①`configure_audio` Idle→Starting 前要求 `deckModeId!=0` 且 deviceHash 非空；②`create_task` 在 `obs_output_create` 前：modeId==0 直接 cooldown 返回，且**对照设备当前有效模式列表校验 modeId**（复用 `obs_get_output_properties`+`obs_property_modified(device)` 枚举，modeId 不在列则 cooldown 返回），杜绝脏 modeId 崩溃；③对话框仅当 modeId!=0 才探测，且 accept 校验路径在「设备已选但无有效模式」时拒绝启用 DeckLink、不持久化 (hash,mode=0)；④`from_obs_data` 对 deckModeId 作基本校验（负值丢弃），与 keyer/audioTrack 对称。

---

## B. 应修复（MED，本轮一并修）

### M1 — 合成尺寸取自持久化快照而非运行时栅格 → 逐帧静默丢弃、不自愈
- 位置：`multiview-instance-serialize-output.cpp:144`（from_obs_data 不对 decklink 强制 resMode=Custom）、`external-output-settings-dialog.cpp:376,386`（仅写路径设 Custom+快照 dims）、丢弃点 `multiview-output-decklink.cpp:432`（`w!=inst->w` 守卫）。
- 场景：(a) 配置 resMode 非 Custom（手编/损坏）→ manager 按画布基准合成、backend 按真实栅格开 video_t → 逐帧丢弃；(b) 持久化 customW/H 与运行时栅格发散（跨机/驱动更新）→ 同样静默黑场。active 却只出黑场，仅一行 WARNING，不自愈。
- 修复：让 backend 对合成尺寸具权威性——backend 从 `obs_output_get_video_conversion` 已知真实栅格，经一个 `IMultiviewOutputBackend` 可选方法把 `inst->w/h` 回报给 manager，Running 后 manager 按之合成（未 Running 时回退 `resolve_output_dimensions`）；同时 from_obs_data 对带 deviceHash 的 decklink 配置强制 resMode=Custom 兜底 (a)。

### M2 — 无存活性检查 → 底层 output 死亡后仍报 Running、不自愈
- 位置：`multiview-output-decklink.cpp:318`（`wants_frame()` 无条件返回 active）、`:338-347`（Running 分支仅对配置变更反应）。
- 场景：拔 SDI 线/输出 error-stop/信号丢失后 backend 无处查 `obs_output_active`，继续合成、环满静默丢弃、`is_active()` 恒 true、无重试无恢复。弱于 NDI（`send_get_no_connections` 检测，`multiview-output-ndi.cpp:62-72`）。
- 修复：`configure_audio` 在 Running 时检测死输出（锁内取 `obs_output_t*`、锁外 `obs_output_active(out)`，或订阅 output `stop` 信号置 SharedState 标志）；检测到停止即取出 inst→Idle+cooldown，走正常重建，取得 NDI 对等自愈。

### M3 — video_t colorspace 硬编码 709，HDR 模式错色 + 逐帧转换
- 位置：`multiview-output-decklink.cpp:233`（`voi.colorspace=VIDEO_CS_709; voi.range=VIDEO_RANGE_FULL`）；对照 `obs-studio/plugins/decklink/decklink-output.cpp:39`（HDR 设备非 force_sdr 时 2100_PQ）。
- 场景：HDR 设备 + force_sdr 关 → OBS 侧转换目标 2100_PQ，backend 以 709 开 video_t → 每帧 709→PQ sws 转换 + 错色。SDR 两侧皆 709/FULL 不受影响。
- 修复：从 `obs_output_get_video_conversion` 读 `conv->colorspace`/`conv->range` 开 video_t（未设回退 709/FULL），SDR/HDR 皆真正零转换。宽高已从 conv 读，补 colorspace/range 即可。

---

## C. 观察项（LOW，已分析接受）
- **L1** `deck*` 对 Spout/NDI 子对象也读写：round-trip 稳定无损坏，仅配置膨胀；设计 §6 已接受共享结构体取舍。可选：按后端门控 deck* 序列化。
- **L2** UI 任务在进程退出时可能不排空 → video_t/audio_t/obs_output 良性泄漏，OS 回收设备、Qt 循环消失晚于模块卸载故无 UAF。可选：最终拆除若已在主线程则同步 `close_output_instance`。
- **L3** 非 Qt/headless（`ui_task_handler==NULL`）下 `obs_queue_task(OBS_TASK_UI)` 静默丢任务、Starting 永卡 + CreateParam 泄漏（`obs.c:3320`）；OBS GUI 合成前已装 handler（`OBSApp.cpp:1275`）故不触发。可选：Starting 记时间戳、超 5s 复位 Idle+cooldown 逃出楔死。

---

## 验证步骤（修复后）
1. clang-format 19.1.1 + gersemi 干净；build Debug + RelWithDebInfo；deploy。
2. **H1 回归**：DeckLink 输出激活出流时，在对话框取消勾选 Enable / 移除实例 / 关窗 / 退出 OBS —— 主节目+预览**零掉帧**（录屏或看 OBS 统计的渲染迟帧计数）。
3. **H2 回归**：设备在场 + modeId=0（组合框空）或脏 modeId 的配置 —— 启用/探测**不崩溃**，干净拒绝 + 日志。
4. **M1**：手编配置把 decklink resMode 改非 custom / 把 customW/H 改错 —— SDI 仍正常出画（backend 栅格权威）或干净降级，不出现 active 黑场。
5. **M2**：出流中拔 SDI 线再插 —— backend 能感知停止并自愈重建（对比修前的恒 Running 黑场）。
6. **M3**：HDR DeckLink 设备（若有）非 force_sdr —— SDI 颜色正确、无逐帧转换告警。
7. 并发压测：反复启用/禁用/改配置/切场景集合 + 与 NDI/Spout 并存。

---

## D. 修复验证（二次对抗审查结果）

对 H1/H2/M1/M2/M3 修复做了 4 路对抗复审 + 综合，独立复核 clang-format/Debug/Rel 均干净。裁决：

| 项 | 裁决 | 依据 |
|---|---|---|
| H1 | **RESOLVED** | 重型 output teardown 全经 `QTimer::singleShot(0,qApp)` 异步化，确认永不内联、不在 `obs_enter_graphics` 锁下跑；`destroy_stages` 内联但仅廉价 GPU 释放。 |
| H2 | **RESOLVED** | 通往 `obs_output_create("decklink_output")` 的所有路径（后端 create_task + 对话框 probe）均先按设备当前 fps 过滤模式表校验 modeId（0/负/拒绝启用全封闭）；该列表是 `outputModeIdMap` 严格子集，不可能命中 `FindOutputMode` 空指针。 |
| M1 | **RESOLVED** | `compose_size()` Running 后以运行态光栅覆盖持久化快照、单锁无死锁；`from_obs_data` 对 decklink 强制 `resMode=Custom`。 |
| M2 | **PARTIAL** | 机制竞态安全、无误报，对**显式 stop / 启动失败**能自愈；但静态审计 obs-studio 表明 `decklink_output` 在**拔线/失信/设备错误时不清 `obs_output_active`**（`ScheduledFrameCompleted` 仅重排下一帧、`obs_output_end_data_capture` 仅由显式 `decklink_output_stop` 调），故「静默拔 SDI 线」场景自愈**休眠**。非回归（严格优于修前）。 |
| M3 | **RESOLVED** | `voi.colorspace/range` 取自 `obs_output_get_video_conversion`，DEFAULT(0) 兜底 709/FULL，与输出转换目标一致，SDR/HDR 皆零逐帧转换。 |

**底线：无遗留发布阻断 HIGH，代码完整可发布，待真机回归。**

### 修复引入的新 LOW（非阻塞；列为后续/全仓审查跟进）
- **LOW-F1** 配置变更重启瞬时 device-busy（`multiview-output-decklink.cpp` 的 Running→Idle 配置变更分支未设 cooldown；旧 unit close 经 QTimer(0) 异步，若未排空就派发新 create 可能撞旧 output 占用设备而失败）。实际 0ms timer 通常早一帧排空、失败也走 3s 重试自愈。**修法**：该分支一并设短 cooldown。
- **LOW-F2** `resMode=Custom` 对任何非空 `deckDeviceHash` 的 backend 强制生效（仅损坏配置把 hash 注入 spout/ndi 时才误伤，不崩溃）。**修法**：按 backend kind 门控（仅 decklink）。
- **LOW-F3** headless/`qApp` 为空时 teardown 泄漏 = 既有 L3，非回归。

### M2 的诚实结论与后续（务必真机验证）
- 真机请**实拔 SDI 线**验证：`decklink_output` 是否会把 `obs_output_active` 置 false。静态结论是**不会**，即 M2 对静默拔线休眠。
- 发布说明据此把 M2 宣称下调为「仅覆盖显式/启动失败 stop 的自愈」，注明静默 SDI 链路丢失在 OBS output 层不可见。
- 若需真正拔线恢复：须在 `obs_output_active` 之外实现——监听 decklink 链路/参考信号，或把 `video_output_lock_frame` 持续 ring-full 停顿当作存活性信号触发 Idle+cooldown 重建。（后续增强，非本轮阻断。）
