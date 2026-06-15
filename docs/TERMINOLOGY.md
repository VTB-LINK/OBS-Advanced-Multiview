# 术语统一规范 / Terminology

> 本文档是 OBS Advanced Multiview 项目的**术语权威基准**。所有文档、commit message、issue、PR 描述、章节标题、代码注释都必须遵循本规范，避免 `Phase` 与 `Milestone` 互换使用或与设计文档内部子里程碑混淆。
>
> 适用范围：仓库内全部 Markdown 文档、新建文档、commit message、PR/issue 标题与描述。
>
> 本文件本身位置：[TERMINOLOGY.md](TERMINOLOGY.md)。如未来需要调整，须先更新本文件，再批量更新引用方。

---

## 1. 引用优先级

当文档之间出现冲突时，按以下优先级裁决，**不允许低优先级文档覆盖高优先级文档**：

1. **PRD 与 general instructions**（Copilot Space 中维护）—— 最高纲领。
2. [../README.md](../README.md) 与 [ROADMAP.md](ROADMAP.md) —— 第二优先级。`ROADMAP.md` 是全局 Milestone 与 Phase 命名的唯一来源。
3. `docs/phase-*.md`、`docs/*-design.md`、`docs/*-hardening-notes.md` 等专题文档 —— 第三优先级。
4. 仓库内其它说明文档（`docs/DEVELOPMENT.md`、`docs/setup/*.md` 等）—— 第四优先级。

新文档必须显式声明自身在该优先级中的位置，避免被误读为权威来源。

---

## 2. 全局 Milestone 命名

全局 Milestone 以 [ROADMAP.md](ROADMAP.md) 为唯一来源，编号 `M0` 到 `M8`：

| 编号 | 中文名 | 主体交付 |
| --- | --- | --- |
| M0 | 仓库整理与基础骨架 | 插件加载、工具菜单、空 Dialog、配置目录 |
| M1 | 配置系统与管理 / 设置 Dialog | JSON 配置、原子保存、实例 CRUD |
| M2 | 布局引擎与编辑网格 | rows/columns/span、gutter、hit-test、动态布局 |
| M3 | MultiviewWindow 与 OBS 内部信号 | 独立窗口、PGM/PRVW/Scene/Source、右键菜单、Source Picker |
| M4 | 视觉参数与辅助功能 | 三层 Visual Settings、Label/Background/SafeArea/Overlay/VU Meter/Highlight |
| M5 | 断开 / 删除 / Signal Lost 行为 | 占位图、黑场、重试、备播、Signal Lost UI |
| M6 | 外部流接入 | NDI、Spout、RTMP、HLS、FLV、SRT、WebRTC |
| M7 | 打包、安装版与便携版 artifacts | Installer、portable zip、配置目录策略 |
| M8 | 性能、稳定性与回归验证 | FPS、降帧、6x6/10x10、source 删除、回归矩阵 |

**写作约定**：

- 正式文档章节标题统一写 `Milestone X：<中文名>`。
- 正文引用允许使用缩写 `M0`、`M3`、`M0~M3`。
- 不允许使用 `第 N 阶段`、`第 N 期`、`Stage N`、`Iter N` 等替代用词。
- 不允许在仓库范围内重新编号 Milestone，例如不允许出现 `Milestone 9`、`Milestone 4.5` 等扩展编号；若需新增阶段性目标，请用下面的 Phase 体系或在 `ROADMAP.md` 中统一扩展。

---

## 3. Phase 命名

Phase 是对 Milestone 的粗粒度阶段分组，固定如下：

| Phase | 范围 | 含义 |
| --- | --- | --- |
| Phase 1 | M0 ~ M3 | 第一个可用 MVP 闭环（OBS 内部信号 Multiview） |
| Phase 2 | M4 | 视觉参数系统主体 |
| Phase 2.5 | M4 收尾 / Phase 3 准备 | **不是新的全局 Milestone**，只是 M4 完成后、M5/M6 开始前的收尾窗口 |
| Phase 3 | M5 ~ M6 | Signal Lost 行为与外部流接入 |
| Phase 4 | M7 ~ M8 | 打包、安装版/便携版与性能稳定性回归 |

**写作约定**：

- 文档中出现 `Phase N` 时，**必须**在首次引用处附带 Milestone 范围，例如 `Phase 1（M0~M3）`、`Phase 2.5（M4 收尾 / Phase 3 准备）`。
- 不允许把 `Phase` 当作 `Milestone` 的同义词使用，例如不允许写 `Phase 0`、`Phase 5` 或 `Phase 4 包含 Milestone 4`。
- Phase 2.5 仅用于阶段性收尾任务（文档同步、验收清单、少量必要 polish），不得在 Phase 2.5 名义下扩张视觉参数功能。

---

## 4. 设计文档内部子里程碑

[phase-2-visual-settings-design.md](phase-2-visual-settings-design.md) 历史上使用过 `Milestone 2.0` ~ `Milestone 2.7` 编号。这些**不是全局 Milestone**，仅是 M4 内部的子任务编号。

**写作约定**：

- 正式引用必须写 `M4 子任务 2.0` 或 `Phase 2 子里程碑 2.0`，避免与全局 `Milestone 2`（布局引擎）混淆。
- 在 [phase-2-visual-settings-design.md](phase-2-visual-settings-design.md) 内部章节标题仍可保留 `Milestone 2.x` 历史编号，但**必须**在文档开头加一段说明，明确这些是 M4 内部子任务而非全局 Milestone。
- 新增任何子里程碑必须遵循 `M<global>.子<sub>` 的两段命名，例如 `M4 子任务 2.8`，不允许直接写 `Milestone 2.8`。

---

## 5. 当前阶段状态（写作时使用的标准表述）

| 编号 / Phase | 状态 | 推荐表述 |
| --- | --- | --- |
| M0 ~ M3 / Phase 1 | 功能闭环已完成；OBS 32.0、tag 构建、Linux/macOS 等验收项未全部确认 | `Phase 1（M0~M3）功能已完成，跨版本/跨平台验收未完全封口` |
| M4 / Phase 2 | 主体功能已完成（Label/Background/SafeArea/Overlay/VU Meter/Highlight、三层 Visual Settings、动态生效） | `Phase 2（M4）主体功能已完成，Phase 2.5 收尾已完成` |
| Phase 2.5 | 已完成（文档重基线、术语统一、Phase 2 验收清单、VU meter polish 设计/实现） | `Phase 2.5（M4 收尾 / Phase 3 准备）已完成` |
| M5 / M6 / Phase 3 | 未开始 | `Phase 3（M5~M6）未开始` |
| M7 / M8 / Phase 4 | 未开始 | `Phase 4（M7~M8）未开始` |

---

## 6. 禁用 / 混用示例

下列写法**禁止**出现在新文档中，已存在的应在后续文档同步中改写：

- `Phase 5` —— Phase 上限为 Phase 4，`Phase 5` 不存在。
- `Milestone 2.5` 指代 Phase 2.5 —— 应写 `Phase 2.5`。
- `第 1 阶段` / `第一阶段` —— 应写 `Phase 1（M0~M3）`。
- `Phase 1 = Milestone 0 + Milestone 1 + Milestone 2 + Milestone 3` —— 简化为 `Phase 1 = M0~M3`，与本规范一致。
- `Milestone 4 包含 Phase 2` —— Phase 与 Milestone 是包含与分组关系，Milestone 不包含 Phase，反向才成立。
- 在 [phase-2-visual-settings-design.md](phase-2-visual-settings-design.md) 之外的文档中独立引用 `Milestone 2.0` ~ `Milestone 2.7` —— 必须加 `M4 子任务` 或 `Phase 2 子里程碑` 限定词。

---

## 7. Commit / PR / Issue 命名建议

- Commit message 与 PR/issue 标题使用纯 ASCII，避免 CI 或镜像服务的编码问题；正文可使用中文。
- 引用 Phase / Milestone 时遵循 `Phase 2.5: ...`、`M4 sub-task 2.6: ...` 等格式。
- 涉及多个 Milestone 时建议显式列出范围，例如 `Phase 3 (M5+M6): NDI scaffold`。

---

## 8. 维护流程

1. 修改任意 Phase / Milestone 定义前，**必须**先更新本文件。
2. 本文件改动应单独 commit，commit message 建议：`docs: update terminology (Phase/Milestone)`。
3. 改动后需在 [../README.md](../README.md) 与 [ROADMAP.md](ROADMAP.md) 中同步引用，确保读者按引用链能找到本文件。
4. 历史文档中的过时写法可分批清理，不强制要求一次性完成；新增内容必须立即遵循本规范。
