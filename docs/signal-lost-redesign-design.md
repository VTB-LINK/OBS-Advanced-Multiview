# 信号丢失系统重构设计(Signal-Lost v2)

> 状态:设计规划中(待评审 + 待定项见 §8)。
> 关联:[issue #5](https://github.com/VTB-LINK/OBS-Advanced-Multiview/issues/5)、
> `docs/issue-5-signal-lost-fallback-reenable-design.md`、
> `docs/phase-3-signal-lost-and-external-sources-design.md`

## 1. 为什么要重构(动机)

审计现有"信号丢失"设置后,发现三类"名不副实",用户心智模型与实际行为对不上:

1. **外部选项的命名暗示"重试行为不同",但实际重试完全一样。**
   health 监督器对外部 cell **无条件运行**,media_restart / recreate 只看 provider 能力 + 状态,
   **与用户选的"丢失时"行为无关**。所以"信号丢失叠加"和"仅重试"的唯一区别是**红带 vs 蓝带**,
   重试方式一模一样。用户以为"信号丢失叠加=纯警告不重试""仅重试=无警告一直重试",都是错的。

2. **三个"重新连接时间"参数坏了。**
   - `初始重试 retryInitialMs` / `最大重试 retryMaxMs`:**死控件**,全代码库只序列化 + 对话框读写,
     运行时从不消费。
   - `重试冷却 manualReconnectCooldownMs`:唯一生效,但被钳到 **≥5s** 且是**固定**冷却,
     **不是**当初设计的指数/阶梯退避。
   - 原始设计意图是退避曲线 `1s→5s→10s→15s→20s→30s(到 max 封顶)`,从未实现。

3. **内部 / 外部语义割裂。**
   "内部源缺失"只有 黑屏/占位图/清除 三项、没有"后备"开关;"后备"分组实际对内部也生效但 UI 没体现。
   用户感觉"内部没法 fallback"即源于此。

**内部检测本身没问题、不需要重做**:pgm/prvw 取 frontend 缓存不会真丢;scene/source 靠
`source_create` 信号 + ~1Hz 弱引用/名字重查恢复很快。内部**不需要 health 监督器**,保持现状。

## 2. 现状速查(供对照)

| 信号类型 | health 监督器 | 自动重试动作 | 恢复方式 |
|---|---|---|---|
| 内部 PGM/PRVW | ❌ | 无 | frontend 缓存,几乎不丢 |
| 内部 Scene/Source | ❌ | 无 | source_create 信号 + ~1Hz 名字重查 |
| 外部 FFmpeg / VLC | ✅ | media_restart + recreate | 自动 |
| 外部 NDI / Spout | ✅ | **无**(restart/recreate 均 false) | 纯等宿主插件(DistroAV / sender)自恢复 |

外部"丢失时"四选项当前**只影响显示**(都在重试):信号丢失叠加=红带、仅重试=蓝带、
重试+后备=后备源、信号丢失图片=静态图。

## 3. 新模型:两个正交轴(内外统一)

把"恢复"和"显示"拆成两个独立的轴,内部和外部 cell **共用同一套模型**。任何 cell 在
**主信号不可用**(内部 missing / 外部 unhealthy)时,行为 = `轴A(恢复策略) × 轴B(显示)`。

```
主信号可用 ──► Active(正常渲染)
主信号不可用 ──► 轴A 决定"后台怎么恢复" + 轴B 决定"这段时间画面显示什么"
```

## 4. 轴 A —— 恢复策略(后台做什么)

| 选项 | 含义 | 适用 |
|---|---|---|
| **自动重连(退避)** | 监督器自动 media_restart→recreate,冷却按退避曲线递增 | FFmpeg / VLC |
| **仅手动(无自动重连)** | 监督器照常**检测 + 显示**丢失,但**不**自动 restart/recreate;只在 Active 才清状态。恢复完全靠用户点 Reconnect/Replay Now | FFmpeg / VLC |
| **自动(由源恢复)** | 我们不做 restart/recreate(NDI/Spout 本就不支持);宿主插件内部自恢复。轴A 选择器在 UI **置灰** | NDI / Spout |
| **事件驱动(由 OBS)** | 内部 cell:靠 source_create + 名字重查恢复,无退避概念 | 内部 |

> **NDI / Spout 的 UI(已定)**:轴A 恢复策略选择器**直接置灰**,并给出明确提示文案,例如
> "NDI / Spout 由源端自动恢复,无需也无法配置重连策略"。不要静默灰掉让用户困惑。

### 4.1 退避曲线(已定:固定阶梯,不暴露给用户)

- **固定阶梯**:第 1..n 次重连冷却 = `5s → 10s → 15s → 20s → 30s`,到 30s 封顶后**保持 30s/次**
  一直重试,直到 Active 或用户手动重连。(步长 +5s,上限 30s。)
- **不暴露给用户**:这条曲线写死为内部常量,UI **不再显示** "初始重试 / 最大重试" 两个控件
  (它们本就是死控件)。
- `kMinRecreateCooldownNs` 的 ≥5s 硬钳取消(改由阶梯的 5s 起点自然保证);Opening 期的
  15s grace / 60s 总超时 / 10s media_restart 间隔等保护性常量保留(防"首帧慢"误判,与用户退避无关)。
- 数据模型里 `retryInitialMs` / `retryMaxMs` 不再被使用:UI 移除,序列化保留读取一个版本周期后清理。

### 4.2 手动 Reconnect / Replay Now(已存在,需补语义)

- 设计初衷:信号已回但退避还在等(最长 30s),用户不该死等 → 立即强制重连。
- 改动:点击时 **重置退避**(`retry_attempt=0`、`next_retry_ns=0`)并**立即触发**一次 restart/recreate。
- 在"仅手动"模式下,这是**唯一**的恢复入口。
- 现有 gating(非 Active 才可点 + cooldown 防抖)保留。

## 5. 轴 B —— 不可用时显示什么(先上全集)

先实现**详细多选项版**,内外共用;测试后再决定删减(见 §8)。拆成两层,可自由组合:

### B1 基础内容(画面主体)
| 选项 | 说明 |
|---|---|
| 黑屏 | 纯黑 |
| 保持最后一帧 | 冻结主信号最后一帧(当前"信号丢失叠加"的隐含行为显式化) |
| 静态图片 | 用户指定图片(拉伸/适配)。**单一图片路径**(已定合并:占位 / 信号丢失 / 后备图三者统一为一个 `lostImagePath`) |
| 后备源 | PGM / PRVW / Scene / Source(走 issue #5 已建的 tracked fallback slot) |
| 清除单元格 | 排期主线程清空该格(仅内部语义,外部一般不用) |

### B2 状态叠加带(覆盖在 B1 之上,可选)
| 选项 | 说明 |
|---|---|
| 无 | 不叠任何文字带 |
| 信号丢失(红) | "SIGNAL LOST" 红带 |
| 重连中(蓝) | "RECONNECTING / CONNECTING..." 蓝带 |
| 自动 | 按轴A/状态自动选(重连中→蓝,放弃/仅手动停滞→红) |

> 现有四个外部选项可表达为 B1×B2 的组合,例如:
> "信号丢失叠加" = 保持最后一帧 + 红带;"仅重试" = 保持最后一帧 + 蓝带;
> "重试+后备" = 后备源 + (无/琥珀 FALLBACK 角标);"信号丢失图片" = 静态图片 + (可选带)。

## 6. 内外统一与 fallback 抽象

- 轴B 的"后备源"对内部和外部走**同一条** tracked fallback slot(issue #5 stage B 已落地的
  `fallback_weak_ref` / `reconcile_fallback_showing` 等),实现用户要的"内外都能 fallback"。
- 内部 cell 的旧 `InternalMissingBehavior`(黑屏/占位/清除)收敛进轴B;外部旧
  `ExternalLostBehavior` 收敛进 轴A + 轴B。

## 7. 配置 / 序列化与迁移

- 新增字段(`LostSignalSettings`):`recoveryPolicy`(轴A)、`displayContent`(B1)、`statusBand`(B2)、
  `lostImagePath`(B1 静态图,合并原 placeholder / signalLost / fallback 三路径)+ 对应单一 `lostImageFitMode`。
- 退避为固定阶梯常量,**不读** `retryInitialMs` / `retryMaxMs`;这两个字段从 UI 移除,序列化保留读取一个版本周期后清理。
- **向后兼容映射**(旧配置 → 新模型),例如:
  - `InternalMissingBehavior::Black` → B1=黑屏, B2=信号丢失(红)
  - `InternalMissingBehavior::PlaceholderImage` → B1=静态图片(用 placeholder 路径)
  - `InternalMissingBehavior::ClearCell` → B1=清除单元格
  - `ExternalLostBehavior::SignalLostOverlay` → A=自动重连, B1=保持最后一帧, B2=红
  - `ExternalLostBehavior::RetryOnly` → A=自动重连, B1=保持最后一帧, B2=蓝
  - `ExternalLostBehavior::RetryWithFallback` → A=自动重连, B1=后备源/静态图(按 fallbackType)
  - `ExternalLostBehavior::SignalLostImage` → A=自动重连, B1=静态图片(用 signalLost 路径)
- 旧字段保留读取一个版本周期,写入只写新字段(渐进迁移)。

## 8. 决策记录(已拍板)

1. ✅ **退避曲线**:固定阶梯 `5/10/15/20/30s`,到 30s 封顶后保持 30s/次;**不暴露给用户**,UI 移除初始/最大重试控件。
2. ⏳ **轴B 全集**:先实现全集,测试后再决定删减(可能不删)。`保持最后一帧` vs `黑屏`、`清除单元格`
   是否对外部开放等,**留到测试后**再判断。(唯一仍开放的项,但不阻塞实现——先全做。)
3. ✅ **图片路径合并**:占位 / 信号丢失 / 后备图 三路径合并为**单一** `lostImagePath`。
4. ✅ **NDI/Spout 轴A**:UI 置灰 + 明确提示文案(不静默)。
5. ✅ **PRVW(issue #5 stage C)**:本次重构**不做**,下次单独处理。

## 9. 与 issue #5 的关系

- stage A(PGM)、stage B(Scene/Source)已完成并部署,引入的 tracked fallback slot 是本重构轴B
  "后备源"的基础设施,直接复用。
- stage C(PRVW 解禁)**不并入本重构**(§8.5),留作下一次单独处理。
- 本重构即用户规划的"全部做完后重新整理 + fallback 能力抽象化"。

## 10. 实现轮廓(非详细,评审后细化)

1. 数据模型:`LostSignalSettings` 加轴A/B1/B2 字段 + 序列化 + 迁移映射。
2. 监督器:退避曲线接 `retryInitialMs/Max`;新增"仅手动"分支(检测但不 restart/recreate);
   去掉 ≥5s 硬钳。
3. 手动重连:Reconnect/Replay Now 重置退避 + 立即触发。
4. 渲染:draw 的状态分类 + overlay 改读轴B(B1 基础内容 + B2 叠加带),内外统一走一套。
5. 对话框:UI 重排成 轴A / 轴B(B1+B2) / 退避参数 三组,按 cell 类型灰掉无意义项。
6. 验证:四 provider × 各轴组合 + 内部 scene/source,装 streamdeck 压测;确认退避曲线、手动重连重置、
   内外 fallback 一致。

---

**最高约束(不变)**:广播级稳定 / 绝不炸 OBS(取数据、还数据都不炸)/ 非阻塞 / 宁可降级显示也不崩。
inc/dec_showing 等会 fire 第三方回调的操作一律主线程锁外执行(见 issue #5 stage B 的 reconcile 模式)。
