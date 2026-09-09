# OBS Advanced Multiview — 全仓架构 + 安全审查（2026-09）

> 范围：系统级设计与安全（过度/欠设计、被忽略的缺口），**明确高于单行代码质量**。
> 方法：4 路独立对抗视角（架构/过度设计、安全/不可信输入、并发/生命周期一致性、跨功能/状态一致性）→ 去重、保守复评。HIGH 仅限真实安全洞 / 不可信输入触发的崩溃类 / 承重且有风险的设计。
> 计数：**HIGH 1 · MED 13 · LOW 7**。本报告为审查快照，未改任何代码；findings 待用户定优先级。

**总评：代码库построen 扎实、纪律良好，核心抽象（LayoutEngine、core/view 拆分、model-owned 序列化、`ISignalProvider` 策略 + 自注册 registry）确有章法，并发/生命周期纪律几乎处处统一。唯一 HIGH 是「加载期从共享配置放大」的健壮性/自我-DoS，而非经典漏洞。其余为 MED 设计债或 LOW 加固。插件并不过度设计；薄弱处反而是个别功能接缝略欠设计。**

---

## 1. 安全 & 健壮性（不可信输入 / 资源 / 崩溃类）

### S1 —[HIGH] 实例数无上限，加载期把无头输出 core + 网络 sender 成片拉到渲染线程
- 位置：`src/config-manager.cpp:147`（load 无上限）→ `plugin-main.cpp:774-777`（FINISHED_LOADING）→ `reconcile_output_host`/`ensure_core`（`plugin-main.cpp:209-234`）→ 每帧 `on_main_rendered`（`:132-143`）。
- 问题：`load_from_file()` 接受 `instances` 数组所有元素、无上限。每个 `outputSettings.any_enabled()` 的实例在加载时自动拉起无头 `AmvInstanceCore` + NDI/Spout/DeckLink sender，随后图形线程**每帧**对每个输出实例跑完整合成+GPU 回读+编码——无窗口、无用户操作。
- 影响：损坏/同步/回滚的 scene-collection 配置（每个后端 enable 就一个 JSON bool）可在启动时驱动无上限图形线程工作 + 网络 sender，拖停直播**主节目**渲染线程、耗尽 GPU/网络。直接违反「绝不阻塞渲染线程/降级不崩」。诚实可达性：配置通常是用户本机文件，不可信向量是共享/同步/回滚的集合而非网络攻击者；判 HIGH 是因「启动即卡死直播」的严重性，而非权限边界。
- 合并自：跨功能「合成开销随输出实例数线性增长、无天花板」（同根因）。
- 修复：load 时给 `instances` 设上限（超 ~32–64 截断/拒绝 + `LOG_WARNING`）；driver 侧再给并发武装的后端数设上限并记降级；考虑把无头 core 创建门控在显式持久化的「输出激活」意图后，使批量 enable 的配置无法成片放大。
- 工作量：M

### S2 —[MED] `SpanRegion` 反序列化只钳下界不钳上界 → 图形线程 layout 计算有符号溢出 UB
- 位置：`multiview-instance.cpp:48`（无上钳）→ `layout-engine.cpp:117`（`r0 + rs > rows` 有符号）。
- 问题：`rowSpan`/`colSpan` 强制 `>=1` 但无上界（不像 `rows`/`columns` 钳到 `[1,20]`）。近 `INT_MAX` 的 span 使 `r0+rs` 溢出（UB）、回绕为负、绕过守卫、以回绕边界驱动累加循环——每次重算都在渲染线程。
- 影响：配置驱动的有符号溢出 UB，位于直播渲染路径。今天实际后果约为乱/漏一格，但它是图形线程上的 UB 且破坏了一致的「反序列化即钳位」契约。
- 修复：`SpanRegion::from_obs_data` 把 span 钳到有界（如 `[1,20]`）；守卫改溢出安全：`if (rs > rows - r0) rs = rows - r0;`。
- 工作量：S

### S3 —[LOW] OBS rescale 导出的输出分辨率绕过了 Custom 模式的 `[16,16384]` 钳位
- 位置：`multiview-instance-serialize-output.cpp:100`（`sscanf "%ux%u"` 仅 `!=0`）→ `resolve_output_dimensions`(237-264) 原样进 `gs_texrender_create`。
- 影响：损坏的 OBS profile ini（`RescaleRes=99999x99999`）可驱动超大 texrender 分配/OOM。已从 MED 下调——需改 OBS 自身 profile ini（比插件 JSON 更可信的源），属待闭合的不对称而非活洞。
- 修复：在 `resolve_output_dimensions` 对所有模式最终解析出的 `(w,h)` 套同样的非零 `[16,16384]` 钳位。工作量：S

### S4 —[LOW] 实例名未钳位且原样作为 NDI/Spout sender 名与窗口标题广播
- 位置：`multiview-instance.cpp:200`（无长度/字符集钳）→ `instance_display_name()` → `NDIlib_send_create`(`ndi.cpp:247`)/`spoutDX::SetSenderName`(`spout.cpp:63`)。
- 影响：多 KB/控制字符名从损坏配置到达 sender-create 与 Qt 标题。低危（局域网标识，非密钥/明显注入点）。与 X1 同一字符串、不同根因。
- 修复：反序列化钳到 ~256、用作 sender 名前剥控制字符。工作量：S

### S5 —[LOW] 每次源刷新对无上限 `cellAssignments` 数组线性扫描
- 位置：`amv-instance-core-sources.cpp:758`/`:236`；数组无上限 `multiview-instance.cpp:232-259`。
- 影响：cell 数有界(≤400)但 assignments 数组无界，每次刷新在 `source_mutex_` 下 O(cellCount×N) 字符串比较。配置大小驱动的软 CPU 成本，非崩溃。
- 修复：load 时按网格 ≤400 上限接受 assignments，或按 `(row,col)` 索引做 O(1)。工作量：S

---

## 2. 架构 & 过度设计（该简化的 + 该加强的）

**主线：仓库有两个结构相同的扩展问题，却用了相反解法——signal 输入用了规范的自注册 registry；输出后端全硬编码。这一不一致是架构维度的主要发现，下列多条 MED 是它的不同侧面，应一并修。**

### A1 —[MED] 输出后端层硬编码（无 registry），后端清单在 ~13 处手工枚举，render 与 teardown 有漂移风险
- 位置：`multiview-output.hpp:141`（enum + 三个命名成员）、两处独立手写 `entries[]`（`.cpp:198`、`:223`）、三分支 `teardown_locked`(277-293)、shutdown 早退 AND 三者(`:302`)、`:73/:86/:99` 平行 switch；加后端还要改 `multiview-instance.hpp:705`、serialize、对话框。
- 影响：render 循环与 teardown/shutdown 各自枚举同一后端集，会漂移且编译器抓不到——某后端加进 render `entries[]` 却漏在 `teardown_locked()` 会静默泄漏其 GPU sender/staging；漏在 shutdown 早退则整个跳过拆解。这是架构项里唯一有具体**正确性**（资源泄漏）后果的。
- 修复（加强）：三成员并为一个 `std::array<BackendEntry,N>`，由单一 `{Kind,factory,availability}` 表构建，所有路径遍历同一容器；理想升级为 `MultiviewOutputBackendRegistry`（镜像 `SignalProviderRegistry`，接口已 ~80% 就绪）。一并退掉 A1+A2。工作量：M

### A2 —[MED] 两个插件点层设计相反（provider registry vs backend 硬编码）——择一统一
- 位置：`signal-provider.hpp:268`/`.cpp:151-154`（自注册）vs `multiview-output.hpp:141`（硬编码）。
- 影响：认知负担 + 保证不一致；后人照先读到的那层抄，传播较弱模式。修复：收敛到 registry（见 A1），**A1+A2 合并为一个工作项**。工作量：M（与 A1 共享）

### A3 —[MED] `OutputBackendSettings` 是个塞了 DeckLink 专属字段的臃肿共享 struct
- 位置：`multiview-instance.hpp:696-699`；`serialize-output.cpp:177-201` 即便 Spout/NDI 也读/钳/force-Custom；UI 镜像 `external-output-settings-dialog.hpp:47-50`。
- 影响：不 scale——每个硬件后端都往共享 struct 塞对他者无用的字段、每个后端仍持久化+校验；`deckDeviceHash`-force-Custom 副作用只对一个后端有意义。修复：后端专属设置拆进 per-backend 子 struct / `variant` / opaque blob（与 A1/A2 registry 化天然配套）。工作量：M

### A4 —[MED] Provider UI 表单分发在两个对话框里逐字重复
- 位置：`edit-source-dialog.cpp:99-131`(+load`:162-181`/read`:192-199`) 与 `source-picker.cpp:350/412/477/524` 各一份 4 路 switch；discovery 是接口外的自由函数(`provider-settings-forms.hpp:188/234`)。
- 影响：加第 5 个 provider（`WebRtcReserved` 已埋桩）要两对话框同步改；某个字段编组修复只改一处会漏 → create 能 round-trip 而 edit 不能的真实分叉 bug。修复：给 `ISignalProvider` 加 `create_settings_form()`/`read_form()`（discovery 并入可选虚函数），两对话框都遍历 `reg.providers()`。工作量：M

### A5 —[MED] 双缓冲 GPU→CPU staging 回读在 NDI 与 DeckLink 各自独立重写
- 位置：`ndi.cpp:88-133`/`:301-302` 与 `decklink.cpp:486-500/:557/:614-615` 逐行平行。Spout 正确地没有（on-GPU share）。
- 影响：经典「改一漏一」重复：resize-ping-pong、map-失败恢复、realloc 守卫都得改两遍；第三个回读消费者会生第三份。修复：抽一个 `StagedReadback` helper 持双缓冲/同步策略与失败日志，后端只实现「给定映射好的 BGRA 帧，发出去」。工作量：M

### A6 —[MED] `amv-instance-core` 是跨 ~11 TU 的 partial class、藏在 492 行 god-header 后、无 aspect 边界
- 位置：`amv-instance-core.hpp:52`（所有 aspect 成员一起声明；一把递归 `source_mutex_`；一个扁平命名空间）。
- 影响：该 header 是几乎每个功能都动的 change-magnet；aspect 拆分仅在编译单元层面，跨 aspect 访问靠约定非编译器。**是债非缺陷**（单锁/单图形线程模型本就合理集中状态）。修复：成员按 aspect 归到子 struct（`VuState`/`HighlightState`/…），跨 aspect 访问变显式可 grep；比真模块化便宜、保住单锁设计。工作量：L

### A7 —[LOW] 状态叠加层：8 个命名成员 + 24 行手工清理，用按 `StatusOverlayKind` 索引的数组即可
- 位置：`amv-instance-core.hpp:310-317`，清理 `status.cpp:172-202`，映射 switch `:330-391`。
- 影响：小拖累；新变体漏在清理块会泄漏一个 text source 到实例销毁。小的**过度设计**点——收成数组。修复：`StatusTextEntry status_[kStatusOverlayKindCount]`，release 变循环，映射 switch 消失。工作量：S

---

## 3. 并发 / 生命周期一致性缺口

**背景：纪律（锁序 `g_registry_mutex → graphics → source_mutex_`、绝不在渲染线程/图形锁下 inc/dec_showing、4 阶段拆解、adopt 语义、跨线程延迟）几乎处处统一。下面两处是偏离了其余所有同类路径都遵守的规则的单独路径。**

### C1 —[MED] 主 lazy 源重解析在渲染线程、持图形锁 + `source_mutex_` 时调 `obs_source_inc_showing`
- 位置：`amv-instance-core-draw.cpp:176`（在 `:62` 起持有的锁内）。
- 问题：其余所有 showing 变更都在 UI/signal 线程或被延迟——Issue #5 fallback 路径(`draw.cpp:415-430`/`source-signals.cpp:279-336`)正是为延迟它而建；主 scene/source 路径绕过了该不变量与它自己建的延迟机制。
- 影响：正常使用可达（绑 Scene/Source 格、删它、~1s 重解析节流内 Undo）：渲染线程持图形锁跑 `inc_showing→activate→enum_active_tree`。今天有界（libobs 把 show 信号延到 video tick），但破坏了声明的不变量、是维护陷阱——未来任何 libobs 改动会在此于渲染线程浮现。
- 修复：主路径比照 fallback——渲染线程只缓存解析出的 weak_ref + 期望显示标志，实际 `inc_showing` 经同款合并 `singleShot(0,this,…)` 投 UI 线程。（或若证明渲染线程 inc_showing 安全，则放宽不变量并简化 fallback——但两路径必须一致。）工作量：M

### C2 —[MED] NDI 后端拆解（`send_destroy`+音频断开）在持 OBS 图形锁时执行；只有 DeckLink 被硬化
- 位置：`multiview-output-ndi.cpp:211-233`；由每帧 `reconcile()`（图形线程，`multiview-output.cpp:127/212-214`）与 `apply_output_settings`（`obs_enter_graphics` 下，`sources.cpp:1224-1233`）调用。DeckLink 正是延迟了这个（`decklink.cpp:508-534`），NDI 没享受同等处理。`NDIlib_send_destroy` 会阻塞到挂起的异步帧发完。
- 影响：禁用 NDI（或应用设置）会阻塞**主节目**图形线程等异步帧 flush；遇慢/拥塞的 NDI 接收端可超一帧 → 直播主节目可见卡顿。与 #16 H1「持图形锁跑阻塞操作」同类，NDI 未处理。（Spout 的 `stop()` 是快 GPU 释放，没问题——分叉专属 NDI。）
- 修复：照搬 DeckLink 模式——只保留 `destroy_stages()` 内联，`send_destroy`+音频断开+`runtime_.reset()` 在锁释放后经 `singleShot` 投 UI 线程（句柄在 `sender_mutex_` 下移进闭包）。工作量：M

### C3 —[LOW] `IMultiviewOutputBackend::stop()` 线程契约文档自相矛盾——这正是 NDI 没被硬化的原因
- 位置：`multiview-output.hpp:87-89` 写「在图形线程调」；`decklink.cpp:520` 注释写「在主线程 obs_enter_graphics 下到达」，漏了每帧 `reconcile()` 调用点。
- 影响：维护者照任一注释都会得错误结论、继续加内联阻塞拆解——它已经酿成 C2。**与 C2 合并**为同一修复的文档半。修复：给出唯一权威契约：`stop()` 可能在图形线程（reconcile）或主线程图形锁下（teardown/apply）→ 绝不内联做阻塞/join/网络 flush；修 DeckLink 注释；拿它审每个后端。工作量：S

---

## 4. 跨功能 & 被忽略场景（「没想到」那一桶）

### X1 —[MED] 无跨实例的共享外部输出资源仲裁（错误的信号上播）
- 位置：sender 名原样取自 `instance_display_name()`（`sources.cpp:1202`）；名字不唯一（加/改名 `manager-dialog.cpp:1147`、克隆 `:1176`、#14 duplicate 原样复制）；无已占用端点的全局登记。
- 影响：两个都叫「Multiview」的实例都开 NDI → 发布两个**同名** sender → 下游切换台可能锁错多画面**上播错信号**（静默的播出正确性故障）。两个 Spout sender 争同一共享纹理注册；两实例在同一 DeckLink 设备硬件冲突且无诊断区分「设备忙」与「配置错」。真·被忽略的交互——设计了 per-instance 隔离，没设计跨实例**资源**争用。
- 修复：进程级已占用端点集（每后端 sender 名、DeckLink device-hash）在 `reconcile()`/`apply_output_settings` 校验，第二占用者拒绝或加后缀重命名并告警；或给每后端一个显式持久化 sender 名字段（默认 `name+uuid8`）替代复用显示名 + 对话框实例名唯一性。（与 S4 互补。）工作量：M

### X2 —[MED] `configVersion` 形同虚设；struct round-trip 静默降级更新版配置
- 位置：`config-manager.cpp:123`（不匹配仅记日志、从不 gate load、从不保留更高版本戳）；全 struct 式 (de)serialize（`multiview-instance.cpp:141+`），未知键下次保存即丢；`save()` 总盖 `CURRENT_CONFIG_VERSION`；`on_scene_collection_changed` 调 `save()`。
- 影响：新版 build 写的 per-collection 配置（回滚、第二个共享配置目录的 OBS 安装、支持降级）在下次切集合时被静默改写——未知字段抹掉、版本就地降级、无提示、只有单个 `.bak`。静默数据丢失。
- 修复：(a) 保留加载的 `obs_data_t`、把 struct 输出合并其上使未知键存活；或 (b) 拒绝盖写 on-disk 版本高于 `CURRENT_CONFIG_VERSION` 的配置（只读加载+告警）；至少保 `max(seen,current)` 使降级可检测。工作量：M

### X3 —[MED] 按 `(row,col)` 键的 per-cell 覆盖/assignment 在网格缩小时不清理、再放大时静默复活
- 位置：`cellAssignments`/`cellVisualSettings`/`cellLostSignalSettings` 按绝对坐标寻址（`sources.cpp:758/:953`）；无论当前网格都序列化（`multiview-instance.cpp:142/166/183`）；缩小不清理。
- 影响：在 `(3,3)` 配覆盖、缩到 2×2 → 条目不可见但保留；之后放回 4×4 → 陈旧视觉/lost-signal/源绑定静默重挂到如今不同的逻辑格（播出面上意外复活状态）。条目还跨 resize 周期无界累积。
- 修复：任何尺寸变更时清理落在新网格外的条目（或引入显式可见的「暂存格」概念）；把覆盖绑到 assignment 身份而非裸坐标也能消歧义。工作量：M

### X4 —[LOW] 尽管有 `configVersion` 脚手架，迁移仍是逐字段临时处理
- 位置：仅一个真迁移（`migrate_lost_settings_v1_to_v2`）内联在 struct；其余是 ~十几个 `from_obs_data` 里散落的 `obs_data_has_user_value` 兜底；钳位/白名单策略不一致；`CURRENT_CONFIG_VERSION` 不驱动任何分发。
- 影响：**欠抽象非过度**。功能持续以独立 schema delta 落地(#5/#10/#11/#14/#16)，易引入在 global/instance/cell 继承层加载不一致的字段。维护驱动、非即时。**与 X2 相关**，下个 schema 改动时一并修。修复：加载后跑一次的单一版本分发迁移步骤；集中钳位/白名单策略使每个标量有定义边界。工作量：M

---

## 5. 值得肯定（确实设计得好，应保留并作为收敛目标）
- **LayoutEngine**——干净、与 OBS 解耦的纯计算模块，被每个 view 与输出 pass 复用。教科书级分离。
- **core/view（多窗口）拆分 + 无头输出 host**——真实、有动机的抽象。
- **model-owned 序列化**——对话框编辑值 struct，**零**手写 `obs_data`；UI↔model 耦合干净不脆。
- **`ISignalProvider` 运行时策略面**（`probe_health`/`supports_media_restart`/`benefits_from_recreate`/`prefers_unbuffered_async`）+ 自注册 `SignalProviderRegistry`——教科书策略模式，集中每 provider 策略。**这是输出层应收敛的目标模型。**
- **反序列化即钳位纪律**——几乎每个标量 load 即钳（dims 16..16384、tracks 1..6、opacity 0..1、path 4096、fontFamily 128），enum 用字符串白名单 + 安全兜底；#16 H2 的 config→OBS-空指针类已守。上面的 findings 正是这套强契约里的少数**不对称**，所以才显眼。
- **并发/生命周期纪律**——锁序一致；frontend 缓存(F2)把 `obs_frontend_*` 挡在渲染线程外；4 阶段「锁内收集/锁外执行」拆解处处统一；`QTimer::singleShot` 延迟总绑 `QObject` 故捕获 `this` 的 lambda 在析构时自动取消；`OBSSourceAutoRelease` adopt 语义正确、**未发现过度释放**；窗口/core/输出拆解顺序四条路径一致。
- **signal-lost 健康状态机**——Active/Paused 与 core 重建时干净重置计数；PRVW+studio-mode-off 降级到 PGM 而非崩溃。
- **DeckLink 拆解硬化**——延迟模式本身**正确**；缺口(C2)是没传播到 NDI，故修复是「套用已有好模式」而非发明。

---

## Top 5 值得做
1. **load 时给实例数设上限 + 门控无头输出成片放大**（S1）。唯一 HIGH；共享配置绝不该能在启动时卡死直播 OBS。—M
2. **span 上界钳位 + 溢出安全的 layout 守卫**（S2）。从直播渲染路径移除有符号溢出 UB；改动极小、回报实在。—S
3. **给 NDI `stop()` 套 DeckLink 的延迟 + 写唯一权威 `stop()` 契约**（C2+C3）。闭合一个直播主节目卡顿 + 造成它的文档矛盾。—M
4. **让配置 round-trip 非破坏性**（X2，带 X4）：保未知键或拒绝盖写更新版。止住降级/共享目录的静默数据丢失。—M
5. **跨实例仲裁外部输出端点**（X1，带 S4）：唯一 sender 身份 + 冲突检测。防「错信号上播」——最高后果的被忽略场景。—M

随后的战略清理：**把输出后端层收敛到 registry**（A1+A2+A3，A5 随之）。解决最大的架构不一致、一次退掉四条 MED。

---

## 裁决：过度 vs 欠设计
**整体均衡，在关乎安全的接缝处略偏*欠*设计——而非镀金。**
- 真·过度设计罕见且局部：8 个命名 status 成员(A7)、臃肿共享设置 struct(A3) 是仅有的「收一收」点，都不危险。
- 真正的弱点是缺护栏而非多机器：无实例上限(S1)、无跨实例资源仲裁(X1)、无配置版本迁移管线(X2/X4)、少数钳位/延迟不对称(S2/C1/C2)。都是*欠*抽象——建了强契约（钳位、延迟 showing、DeckLink 拆解）却把几条路径留在契约外。
- 唯一真不对称是「不一致」而非「数量」：provider 层抽象良好而输出层硬编码(A1/A2)。收敛它们消除最大的未来缺陷债来源。

净：成熟、有纪律的代码库，下一步改进是*加固自己好模式的边缘*，不是拆抽象。做掉上面五项，系统性风险画像显著下降；架构清理可按自身节奏跟进。
