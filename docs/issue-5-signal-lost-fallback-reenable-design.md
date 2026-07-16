# Issue #5 — 安全地重新启用 Signal Lost fallback (PGM / PRVW / Scene / Source)

> 状态:设计已评审通过,待实现。
> 关联:[issue #5](https://github.com/VTB-LINK/OBS-Advanced-Multiview/issues/5)、`docs/phase-3-signal-lost-and-external-sources-design.md`、`docs/obs-isolation-review.md`

## 1. 背景与问题

Signal Lost 的 fallback 备用源共有 6 个选项,但其中 4 个基于 OBS 内部源的选项
—— **PGM / PRVW / Scene / Source** —— 在 `src/signal-lost-settings-dialog.cpp` 的
`kFallbackOptions[]` 被标记 `enabled=false`、UI 灰掉不可选(通过清除底层
`QStandardItemModel` 的 `ItemIsEnabled / ItemIsSelectable`)。渲染路径仍认这些 token,
以兼容此前已存盘的配置。

### 崩溃根因(已确认)

fallback 渲染时调用 `obs_source_video_render(scene)` 会下沉到 libobs 的
`scene_video_render → update_transforms_and_prune_sources`,触发其它插件(尤其
**streamdeck-plugin-obs**)的 sceneitem 信号回调。当某个 scene 正被 OBS 拆除、而第三方插件
持有过期 sceneitem 状态时,**我们的渲染线程**与 **OBS 主线程的移除流程**并发触发同一批
第三方回调,导致崩溃(典型栈 `signal_handler_signal+0x122`)。我们的 `source_mutex_`
无法约束第三方插件,因此挡不住这个跨线程 race。

### 关键洞察

同样这 4 种类型,作为**主源(primary)cell** 并没有被禁用、且生产稳定。被禁的只是
**fallback 配置**。差别在于:主源 cell 经过 M5.4 hardening 已具备完整的生命周期纪律,而
fallback 路径几乎裸奔——尤其 scene/source fallback 在 `draw_cells()` 里**每帧**
`obs_get_source_by_name()` 现查一个**无跟踪、无 `inc_showing`、不接入移除回调**的活引用。

> **结论:安全重启 = 给 fallback 槽补齐"主源同款"的生命周期纪律,把它拉到与主源同等的安全级别。**

## 2. 最高约束(贯穿所有决策,不可违背)

本插件一贯的设计目标,任何方案先满足这些再谈功能:

- **稳定性 / 鲁棒性**:绝不能炸 OBS——无论从 OBS 取数据(渲染 fallback 源)还是把数据交还 OBS。
- **非阻塞**:渲染线程不可被阻塞;会 fire 第三方回调的操作(`dec_showing` 等)**绝不能在
  `source_mutex_` 持锁时做**。
- **广播级稳定性**:宁可降级显示警告文字,也绝不崩。
- **不递归 fallback**(`phase-3` §357:fallback 自身失败不再 fallback)。

## 3. 三阶段发布

| 阶段 | 范围 | PR | 风险 |
|---|---|---|---|
| **A** | 仅 PGM 解禁 | 独立 PR,先合 | 零(走 `obs_render_main_texture`,不下沉 scene 渲染) |
| **B** | scene/source 解禁 + 双源槽生命周期改造 | 随后 PR | 改造后等同主源(已发布、稳定) |
| **C** | PRVW 最后单独解禁 | scene/source 压测稳定后 | 低(活预览场景),最保守 |

### 阶段 A — PGM(零风险,可独立先发)

PGM fallback 设 `isPgm=true`,渲染走 `obs_render_main_texture()` —— 画的是 OBS 已合成好的
主输出纹理,**不调 `obs_source_video_render()`、不下沉 scene 渲染、不触发任何 sceneitem 回调**,
崩溃栈根本不可达。来源是主线程刷新的 frontend 缓存(`src/amv-frontend-cache.cpp`),无悬空。
状态映射(FallbackActive → 琥珀 FALLBACK 角标)已就绪。

**改动:** 仅把 `kFallbackOptions[]` 里 pgm 行的 `enabled` 改为 `true`。**无任何渲染路径改动。**

### 阶段 B — scene/source(核心改造)

落地下面"双源槽生命周期"全部内容后,把 scene/source 行 `enabled` 改为 `true`。

### 阶段 C — PRVW(最后)

scene/source 在压测中确认稳定后,把 prvw 行 `enabled` 改为 `true`。PRVW 走
`obs_source_video_render()` 渲染预览场景(无 main-texture 捷径),有低概率残余风险,但来源是
frontend 缓存的**活**预览场景(概率远低于任意名现查),且已有 `obs_source_removed` 守卫。
**prvw 不需要单独的 weak_ref 槽**(它本就是 frontend 缓存的无名活引用),只需纳入统一的渲染前守卫。

## 4. 双源槽生命周期模型(阶段 B 核心)

把每个 cell 理解为有两个对称的源槽:**primary 槽**(已有,稳定)+ **fallback 槽**(本次新增)。
fallback 槽精确复刻 primary 已被验证安全的**非对称 `dec_showing` 策略**。

### CellSource 新增字段(`src/amv-instance-core.hpp`,CellSource 结构体)

```cpp
OBSWeakSource fallback_weak_ref;    /* 仅 scene/source fallback 缓存(prvw/pgm 不用) */
bool fallback_showing = false;      /* 与 inc/dec_showing 配对,移除时非对称处理 */
bool fallback_active = false;       /* 当前帧是否正在画 fallback,用于 inc/dec 边沿检测 */
std::string resolved_fallback_name; /* 解析时的名字,供 rename / lazy re-resolve 重查 */
```

### 四事件处理(对照 primary 范本)

| 事件 | primary 范本(`amv-instance-core-sources.cpp` / `-draw.cpp`) | fallback 槽处理 |
|---|---|---|
| **resolve** | `update_source_refs()` scene/source 绑定 | refresh 时(**非每帧**)解析 `eff.fallbackName` → 缓存 `fallback_weak_ref`;**此处不 inc_showing**(见 §6 showing 时机) |
| **remove** | `on_source_being_removed()` 命中后只置空、**不 dec** | 命中 `fallback_weak_ref==source` → 置空 + `fallback_showing=false` + `fallback_active=false`,**同样不 dec_showing**,**不改 state**(让下一帧 draw 瀑布重新分类);**不**触发 ClearCell |
| **re-resolve** | `draw_cells()` 节流按名重查(`re_resolve_counter_`) | 复用 `re_resolve_counter_`:`fallback_weak_ref` 空 && `resolved_fallback_name` 非空 && `re_resolve_counter_==0` 时按名重查 |
| **teardown** | `release_source_refs()` **锁外** dec | `fallback_showing` 时**锁外** `dec_showing`(collect-under-lock / dec-outside-lock) |

### 非对称 `dec_showing` 的依据(核心,务必遵守)

- **移除同步窗口里不 dec**——`dec_showing` 会同步 fire "hide" 信号进所有第三方插件,而此刻源正被
  OBS 拆除(已 removed、未销毁),正是 #5 崩溃的同类时机;且源销毁会连同 show_refs 一起释放,
  **无泄漏**(依据 `on_source_being_removed()` 现有注释)。
- **主动解绑 / 切回时在锁外 dec**——此时源活着且稳定,`dec_showing` 会 fire scene callbacks,
  不能持 `source_mutex_`(依据 `update_source_refs()` Phase 2 现有注释)。

## 5. 安全渲染路径(`amv-instance-core-draw.cpp` fallback 解析块改造)

把"每帧 `obs_get_source_by_name`"改为"缓存 weak_ref + `OBSGetStrongRef` + 双重 `obs_source_removed` 守卫":

```cpp
} else if (ft == "scene" || ft == "source") {
    if (cs.fallback_weak_ref)
        srcHolder = OBSGetStrongRef(cs.fallback_weak_ref);   // 受 remove 回调守护
    // (空 && resolved_fallback_name 非空 && re_resolve_counter_==0 → 节流 lazy re-resolve)
    if (srcHolder && !obs_source_removed(srcHolder)) {        // 同主源渲染前守卫
        src = srcHolder; isFallback = true;
    } else {
        srcHolder = nullptr;                                  // 自然降级
    }
}
```

这把 fallback 拉到与主源同等安全级别:同一缓存机制、同一移除守护(`on_source_being_removed`
命中即置空,后续帧 `OBSGetStrongRef` 返回 null)、同一渲染前 `obs_source_removed` 守卫。
移除同步窗口里 fallback 不再持有"绕过守护"的活句柄。

## 6. showing 引用时机(唯一真正新增的簿记)

primary 是"绑定即 showing,拆除才 dec";fallback **不能照搬**——待命的 fallback 常指向
media/browser 源,一解析就 inc 会让它在 primary 健康时长期后台解码,浪费资源、违背"待命"语义
(呼应 `phase-3` §360"切回时释放 fallback showing ref")。故 fallback 的 showing 随
`fallback_active` 边沿开关:

- 进入 FallbackActive(`fallback_active` false→true):`inc_showing` —— **锁外**
- 退出 FallbackActive(primary 恢复):`dec_showing` —— **锁外**
- cell 拆除:`fallback_showing` 时 `dec_showing` —— **锁外**
- fallback 目标被 `source_remove`:只置空,**不 dec**

**实现方式(满足非阻塞约束):** 边沿在 `draw_cells()`(渲染线程,持 `source_mutex_`)里检出,
**不可**就地 inc/dec。采用 **deferred-flag + 主线程锁外执行**,与现有
`volmeters_rebuild_requested_`、ClearCell 的 `QTimer::singleShot(0, this, …)` 模式一致:
draw 里只标记 pending,主线程 tick 取出、在锁外执行 inc/dec_showing。

## 7. 三级状态瀑布(降级语义)

| primary | fallback | 状态 | 画面 |
|---|---|---|---|
| 活 | — | `Active` | 主源,无角标 |
| 死 | 活 | `FallbackActive` | fallback 画面 + 琥珀 "FALLBACK" 角标 |
| 死 | 死 | `MissingInternal` → `MissingScene` / `SignalLost` | 红色警告带 |

现有 state 分类(`draw_cells()` 内部分类块)已天然满足:`isFallback` 真 → FallbackActive;
否则无 src → MissingInternal。overlay 映射(`amv-instance-core-status.cpp`:Fallback / Missing /
SignalLost)已完整。

**fallback 目标被删 → 自然降级到警告文字,无需新渲染代码:** weak_ref 被 remove 回调置空 →
下一帧 `OBSGetStrongRef` 返回 null → `isFallback` 不置真 → 落 MissingInternal → 命中现有
MISSING SOURCE 兜底分支。纯靠瀑布,且天然不递归 fallback。

## 8. on_source_being_removed() 改造

现有循环只检查 primary `cs.weak_ref`。新增 fallback 命中分支,与 primary 完全同款:

- 命中 `cs.fallback_weak_ref == source` → 置空 `fallback_weak_ref` + `fallback_showing=false`
  + `fallback_active=false`,**绝不 dec_showing**,**不改 cs.state**(下一帧 draw 瀑布会把它从
  FallbackActive 降级为 MissingInternal)。
- fallback 命中**不**进入 ClearCell 路径(ClearCell 是 primary 源消失的语义)。
- 一个源同时是某 cell 的 primary 和另一处的 fallback 时,两个分支各自独立命中、各自置空。

## 9. 边界 case

- **fallback 指向自己 / 与 primary 同源**:primary 死意味着该源已没,按名解析拿到同一(removed/null)
  源 → `obs_source_removed` 拦下 → 降级,不循环。
- **fallback 指向另一个也在多视图里的源**:show_refs 是计数器,多处 inc 安全;各 cell 独立跟踪。
- **fallbackName 为空**:现有守卫已拦,src 保持 null → 降级。
- **外部 provider cell 的 fallback**:已有 `fallback_latched` 在 src 被 drop 后走同一解析块;
  新缓存路径统一生效,inc_showing 仍受 latch + `fallback_active` 边沿控制。

## 10. 改动文件清单

1. `src/signal-lost-settings-dialog.cpp` —— `kFallbackOptions[]` 分阶段把 enabled 开关:A 开 pgm,B 开 scene/source,C 开 prvw。
2. `src/amv-instance-core.hpp` —— CellSource 新增 4 字段;如需主线程 tick 取 deferred showing,声明对应 pending 标志。
3. `src/amv-instance-core-draw.cpp` —— fallback 解析块改缓存解析 + `OBSGetStrongRef` 守卫;FallbackActive 边沿检测置 deferred showing flag;prvw 纳入统一渲染前守卫。
4. `src/amv-instance-core-sources.cpp` —— `on_source_being_removed` 加 fallback 命中分支;`update_source_refs` / `update_source_refs_lazy` / `refresh_cell` 解析并缓存 fallback weak_ref;`release_source_refs` 锁外 dec fallback showing;主线程 tick 执行 deferred inc/dec_showing;(可选)`on_source_just_created` 重绑 fallback。

   > **文件位置更新(本设计落地之后的重构)**:`refactor: split OBS source-signal bridge` 已把
   > `on_source_being_removed` / `on_source_just_created` / `update_source_refs_lazy` /
   > `refresh_sources_lazy` / `reconcile_fallback_showing` **逐字搬到**
   > `src/amv-instance-core-source-signals.cpp`(行为未变)。上面第 4 条里属于这几个方法的改动,
   > 现在请到该文件查看;其余(`update_source_refs` / `refresh_cell` / `release_source_refs`)
   > 仍在 `amv-instance-core-sources.cpp`。

5. `src/amv-instance-core-status.cpp` —— 映射已就绪,通常无需改;确认 FallbackActive 角标在 fallback 命中时正确显示。

## 11. 验证(端到端)

**前置:必须安装 streamdeck-plugin-obs**(崩溃复现的关键第三方插件)。

1. **阶段 A 基线**:配 PGM fallback,反复切换/删除场景 + 让 primary 反复掉线触发 FallbackActive。
   预期:零崩溃(基线)。
2. **阶段 B 压测**:配 scene/source primary + scene fallback。反复 remove/restore(Undo)
   **fallback 目标源**,同时让 primary 反复掉线触发 FallbackActive;辅以高频脚本制造并发窗口。观察:
   - 无 `signal_handler_signal+0x122` 崩溃;
   - 角标在 Active / FALLBACK / MISSING 间切换正确;
   - fallback 目标被删 → 立刻降级到红色警告(不卡死、不崩);目标恢复 → 自动回到 FALLBACK;
   - 无 show_refs 泄漏(用 OBS 日志 / 引用计数核对);
   - **primary 健康时,待命 fallback 源 CPU 占用为 0**(验证 inc_showing 仅在 FallbackActive 时发生)。
3. **阶段 C**:scene/source 压测稳定后,对 prvw 重复上述并发压测。
4. **回归**:运行现有 CI(本地先跑 clang-format 19.1.1 + gersemi 对齐格式)。
