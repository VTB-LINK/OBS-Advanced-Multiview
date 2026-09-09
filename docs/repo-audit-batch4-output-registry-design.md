# 全仓审查 · 批 4 设计文档 —— 输出层收敛到 registry（A1 + A2 + A3 + A5）

状态：设计待评审（checkpoint），未动代码
来源：`docs/repo-audit-arch-security-2026-09.md` §2（A1/A2/A3/A5）
前置：批 1-3（S/C/X）已提交（`db7749e`）。本批是**纯可维护性重构，非 correctness 修复**；审查裁决称架构清理「可按自身节奏」。

---

## 1. 动机（要解决的系统性不一致）

仓库有两个结构相同的「插件点」层，却用了相反设计：
- **输入 provider**：规范的自注册 `SignalProviderRegistry` + `ISignalProvider` 富策略接口（`signal-provider.hpp:53/268`）。加 provider = 实现接口 + 注册一处。
- **输出 backend**：**全硬编码**。加一个后端要改 ~13 处：`Kind` 枚举、三个命名成员 `spout_/ndi_/decklink_`、**两处独立手写 `entries[]` 数组**、`teardown_locked` 三分支、`shutdown_graphics` 空判 AND、`backend_available`/`create_backend`/`kind_name` 三个 switch、`InstanceOutputSettings` 命名字段、serialize、dialog。

后果（审查 A1，唯一有**正确性**后果的架构项）：render 循环与 teardown 各自枚举同一后端集，可**漂移且编译器抓不到**——某后端加进 render `entries[]` 却漏在 `teardown_locked` → 静默泄漏其 GPU sender/staging；漏在 shutdown 空判 → 整个跳过拆解。#16 加 DeckLink 时正是逐处手改、靠人肉不漏。

伴随：
- **A3**：`OutputBackendSettings` 是胖共享 struct，塞了 DeckLink 专属字段 `deckDeviceHash/deckModeId/deckKeyer/deckForceSdr`，Spout/NDI 也持久化/校验它们，`deckDeviceHash`-force-Custom 副作用只对一个后端有意义。每加一个硬件后端就继续膨胀。
- **A5**：双缓冲 GPU→CPU staging 回读在 NDI 与 DeckLink **各自独立重写**（`multiview-output-ndi.cpp` 与 `-decklink.cpp` 逐行平行）。改一漏一风险；第三个回读消费者会生第三份。

---

## 2. 目标设计

### 2.1 输出后端 registry（A1 + A2）
镜像 `SignalProviderRegistry`，但适配输出后端是**有状态运行实例**（不同于无状态输入策略）：registry 只持有**种类描述符**，manager 仍持有 live 实例。

```cpp
// 种类描述符：静态、无状态、每 Kind 一条
struct OutputBackendDesc {
    OutputBackendKind kind;                 // enum { Spout, Ndi, Decklink }
    const char *id;                         // "spout"/"ndi"/"decklink"（= serialize 键 + 日志名）
    bool (*available)();                    // = 现 spout_supported/ndi_supported/decklink_supported
    std::unique_ptr<IMultiviewOutputBackend> (*create)();  // = 现 create_*_output_backend
    bool supportsAudio;                     // 供 dialog 灰置音频控件
    // display-name 键等元数据按需
};

// 单一注册表（编译期静态表即可，无需运行时自注册——后端集在编译期固定，
// 用 #ifdef AMV_ENABLE_* 条件收录；比输入 provider 的运行时自注册更简单且足够）
const std::vector<OutputBackendDesc> &output_backend_registry();
```

Manager 改为**单一容器**替代三命名成员 + 两 entries[] + 三 switch：
```cpp
struct BackendEntry { OutputBackendKind kind; std::unique_ptr<IMultiviewOutputBackend> backend; bool enabled; uint32_t w,h; int fpsDivisor; uint64_t frame; };
std::vector<BackendEntry> backends_;   // 每个 registry 描述符一条，构造时按 registry 建
```
`reconcile`/`render_one_resolution`/`render_all`/`teardown_locked`/`shutdown_graphics` 全部**遍历 `backends_`**。加后端 = registry 加一条 → 所有路径自动覆盖，**entries[] 漏改隐患结构性消除**。

### 2.2 每后端设置拆分（A3）
- 瘦身公共 `OutputBackendSettings`：保留所有后端共有字段（enabled/resMode/customW/H/fpsDivisor/audioMode/audioTrackIndex）。
- DeckLink 专属字段移入 `DeckLinkBackendSettings { deviceHash; modeId; keyer; forceSdr; }`（后端专属子结构）。
- `InstanceOutputSettings` 由三命名成员改为**按 kind 键的容器**（`std::map<OutputBackendKind, OutputBackendSettings>` 或按 registry 顺序的 vector），serialize 遍历 registry 用 `desc.id` 作键（取代 `"spout"/"ndi"/"decklink"` 硬编码三段）；DeckLink 子结构单独 nest。
- backend 经 `configure_audio(cfg)`（现有通道）拿公共设置；DeckLink 额外经一个 kind-specific 通道拿 `DeckLinkBackendSettings`（新增一个可选虚方法或在描述符里携带解析）。
- `deckDeviceHash`-force-Custom 的副作用收敛进 DeckLink 自己的解析,不再污染公共 from_obs_data。

### 2.3 StagedReadback helper（A5）
抽一个持「双缓冲 ping-pong + gs_stage/map + 失败告警」策略的 helper：
```cpp
class StagedReadback {
    // ensure(w,h,double_buffer); stage(tex); 返回本帧可读的 mapped BGRA(data,linesize) 或 nullptr(未就绪/环空);
    // destroy(); 内含 stage_[2]/idx/have_prev/warned 状态。
};
```
NDI/DeckLink 后端各持一个 `StagedReadback`，只实现「给定 mapped BGRA 帧,发出去」的尾部（NDI: send_video；DeckLink: video_output_lock_frame+memcpy）。Spout 无回读、不涉及。

---

## 3. 迁移（分可独立验收的子步；每步 build + 对抗审查 + 提交）

- **4a — Manager 后端 registry（A1+A2）**：加 `OutputBackendDesc`/`output_backend_registry()`；manager 三命名成员+两 entries[]+三 switch → 单一 `backends_` 容器 + registry 驱动；`InstanceOutputSettings` 暂保命名成员，manager 经 `settings_for(kind)`（**唯一**一处 kind→settings 映射）取。**行为逐字保持**（reconcile/render/teardown 语义不变，只是遍历方式变）。退掉 A1 的 entries[] 漏改隐患 + A2。
- **4b — 设置容器 + 拆分（A3）**：`InstanceOutputSettings` → kind 键容器；serialize 遍历 registry；`DeckLinkBackendSettings` 拆出;dialog 适配（tab 列表由 registry 驱动，但每后端 widget 仍 bespoke——DeckLink 的 device/mode 是专属的,这不属本批的「表单泛化」A4）。消掉 4a 里残留的 `settings_for` switch。
- **4c — StagedReadback helper（A5）**：抽 helper，NDI/DeckLink 改用。逐字搬双缓冲逻辑,只余尾部差异。

可在任一子步停下（每步自洽、可提交、可发版）。

---

## 4. 行为保持 & 崩溃面（重构红线）

这是**行为保持重构**，必须逐字保住批 1-3/#16 的全部已加固不变式，审查须逐条核对「重构前后语义等价」：
- **线程/锁（C1/C2/C3、#16 H1）**：`stop()` 契约（图形线程 reconcile 或主线程持图形锁）不变;重拆解仍经 `invokeMethod(qApp,QueuedConnection)` UI 线程延迟 + 退出 drain;`submit_frame`/readback 仍在图形线程;registry 遍历不得引入新锁或改变 reconcile/teardown 的线程上下文。
- **entries[] 覆盖**：新单容器必须让 render **和** teardown **和** shutdown 空判 **和** prepare/wants_frame **全部**遍历同一 `backends_`——这正是本批要害,审查须确认无路径漏遍历。
- **backend-authoritative compose size（M1）**、**S3 分辨率钳位**、**S1 armed 上限**（`plugin-main` 数后端数的逻辑要适配新容器/settings 形状）、DeckLink 的 mode_id 校验/UI 线程生命周期/silent audio 等 #16 硬化**全部保留**。
- **序列化向后兼容（X2 + 既有）**：4b 改 serialize 键遍历后,**旧配置（"spout"/"ndi"/"decklink"/"decklink" 子对象）必须无损加载**;X2 版本守卫不受影响;round-trip 稳定。这是 4b 最大风险面。
- **DeckLink 已提交在 #16/批2/批3**：4a/4b/4c 会再动 `multiview-output-decklink.cpp`——不得回退其 H1/mode 校验/compose_size。

## 5. 风险与缓解
- **重构刚加固的代码可能回退 bug**（本批唯一实质风险）。缓解：①每子步行为逐字保持、机械搬移;②每子步 build(Debug+Rel) + **对抗审查逐条核语义等价 + 多举反例**(尤其 entries[] 全覆盖、序列化向后兼容、线程契约);③子步独立提交,可随时停。
- **收益 vs 代价**：退 4 条 MED + 结构性消除泄漏隐患 vs 重构风险。审查裁决支持做,但属「自身节奏」——故分子步、可中止。

## 6. 明确不在本批（独立后续）
- **A6**（`amv-instance-core` god-header → per-aspect 子结构,L 级大改）——单独评估。
- **A7**（状态叠加层 8 命名成员 → 数组,LOW 小改）——可随手做,但不属输出层收敛。
- **A4**（provider 表单在两对话框重复 → `ISignalProvider` 加 create_form/read_form）——输入侧,非本批。

## 7. 验证（每子步）
- clang-format + gersemi(若动 CMake)干净;build Debug + RelWithDebInfo;deploy(可选)。
- 对抗审查：语义等价（reconcile/render/teardown 逐路径）、entries[] 全覆盖无漏、序列化向后兼容 round-trip、线程契约保持、所有 S/C/X/#16 不变式未回退。
- 真机（有条件）：NDI/Spout/DeckLink 启用/禁用/并存、切场景集合、关窗/退出,行为与重构前一致、无泄漏无卡顿。
