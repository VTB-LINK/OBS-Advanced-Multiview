# Signal-Lost v2 — 发版前硬化笔记

> 范围:Signal-Lost v2(退避阶梯 / 恢复策略 / 统一显示模型 / tracked fallback slot /
> PRVW / NDI·Spout 检测)。三路并行审计(并发·引用计数·锁序 / 数据模型桥接 / 退避状态机)
> 后的修复与观察。关联:`docs/signal-lost-redesign-design.md`、issue #5。

## 总评

发版**阻断级缺陷:无**。未发现 UAF、过度释放、锁序倒置或会阻塞/炸 OBS 的路径。
fallback slot 的不对称 remove 纪律(命中只置空、不 dec_showing,靠源销毁释放 show_refs)与
既有 primary weak_ref 纪律严格对称——该设计最稳的部分。修复 1 个 HIGH + 3 个防御性补强。

## 修复清单(本次硬化)

| 文件 | 修复 | 级别 |
|---|---|---|
| `amv-instance-core-sources.cpp` | **H1**:`force_reconnect_cell` 手动重连时漏 reset `lost_since_ns`。导致手动尝试失败后,Lost 分支的初始等待门 `lost_for < cooldown` 因 `lost_for` 已很大而被旁路,监督器在下一个 ~1s tick 立即重试,**第一级 5s 退避被跳过**(用户感知为"手动点一下后疯狂重试")。修复:同步 `cs.lost_since_ns = 0`,使下次进入 Lost 由 `if (lost_since_ns==0) lost_since_ns=now` 重新计时。 | HIGH |
| `multiview-instance-serialize-signal.cpp` | **M-2**:`derive_legacy_lost_fields` 对 `displayContent=Fallback` 但**无可用目标**(fallbackType 空,或 image 且 path 空)会派生出矛盾的 legacy 态(RetryWithFallback 却无内容)。对话框走不到,但手改 JSON / 迁移可能产生。修复:派生时把"不可用 fallback"按 Black 处理(`effContent`),`displayContent` 本身保留不动。 | MED |
| `multiview-instance-serialize-signal.cpp` | **M-3**:迁移旧 `SignalLostImage` / `PlaceholderImage` 但图片路径为空时,会生成 `Fallback + image + 空路径`。修复:空路径降级回 `Black`,保持存储态干净。 | MED |
| `multiview-instance-serialize-signal.cpp` | **L-2**:`migrate_*` 把 legacy 图片路径搬进 `fallbackName` 发生在 `from_obs_data` 的 fallbackName 钳位之后。修复:迁移末尾对 `fallbackName` 再钳一次 4096(纵深防御)。 | LOW |

## 观察项(已分析,本次不改)

- **reconcile 乐观提交的自愈型 showing 泄漏窗口**(`reconcile_fallback_showing`):锁内"提交
  `fallback_shown_ref = desired`"早于锁外执行 inc/dec。极窄三重窗口(提交后、inc 执行前、
  primary 恢复且 fallback 源恰被移除)下可能净 +1 showing。**不 UAF、不过度释放**,源销毁时
  libobs 随 show_refs 一并回收;`on_source_being_removed` 命中清 `shown_ref` + inc/dec 的
  `obs_source_removed` 守卫进一步收窄。**乐观提交是为了让重叠 reconcile 不重复 inc**,移除它会
  引入 double-inc 风险,故保留。后续若要消除:把 `shown_ref` 的提交移到 inc/dec 成功之后并 CAS。
- **退避阶梯 5/10/15/20/30 跨 restart+recreate 共享 `recovery_attempt`**:经实测日志确认产出正是
  `5→10→15→20→30`(3 次 cheap restart 消耗 5/10/15,recreate 落在 20,下一次 recreate 30s 封顶),
  **符合设计**,非缺陷。
- **restart-only provider(supports_media_restart && !benefits_from_recreate)的 `recovery_attempt`
  不回绕**:持续掉线会收敛到 30s 稳态——合理。当前无此类 provider(FFmpeg/VLC 两者皆 true,
  NDI/Spout 皆 false 早返回),理论路径,记录备查。
- **AmvInstanceCore 的 QObject 线程亲和性不变量**:`QTimer::singleShot(0, this, ...)` 在渲染线程
  持锁投递安全(只入队不同步执行),且 core 析构在主线程会取消未决事件——**前提是 core 在主线程
  构造**(`ensure_core` 主线程调用,当前成立)。若未来出现非主线程 `new AmvInstanceCore`,会破坏此
  保证。建议后续在构造处加 `assert(QThread::currentThread()==qApp->thread())` 固化。
- **`release_source_refs` 持 `source_mutex_` 调 `dec_showing`**:理论上违反"绝不在持锁时 inc/dec_
  showing"铁律,但仅在**全量 teardown**(源稳定、无第三方 hide 反向取锁)发生,且 primary weak_ref
  历来如此,fallback 只是延续既有模式。受控例外,记录备查(运行时态的 `refresh_cell` 则正确走锁外
  collect/dec)。

## 验证

- 退避诊断日志 `ladder step N/5, after Xs wait` 应见 5→10→15→20→30。
- **H1 回归**:外部源 Lost 中点 Reconnect Now,若仍连不上,监督器下一次自动重试应等满 ~5s(而非 ~1s)。
- 装 streamdeck-plugin-obs 反复 remove/restore fallback 目标源 + primary 反复掉线压并发,确认无
  `signal_handler_signal+0x122`、无 show_refs 泄漏、角标切换正确。
