# 每格旋转（Rotation）发版前硬化笔记

对应改动：分支 `feat/cell-rotation`，两个提交
- `feat: per-cell orthogonal rotation of the signal video (90/180/270)`
- `fix: show groups in the source picker "Sources" tab`

## 总评

**无阻断缺陷（无 HIGH/MED）。** 旋转是纯 GPU 投影/矩阵改动，安全画像与既有镜像特性同构：
图形线程本地、`gs_matrix` push/pop 严格平衡、零锁、零 `obs_source` 引用计数增减、无 OBS 回调重入。
群组修复不引入新的生命周期风险——群组在 OBS 内部就是 SCENE 型源，其 `inc/dec_showing` 遍历/回调面与
既有「场景」型 cell 完全一致，继承其已验证的安全性。

对抗审计发现 1 条 LOW（诊断日志自洽性），已在 `feat` 提交内修复。

## 审计方法

按 AGENTS.md §5，用后台 workflow 并行派 4 个独立视角的 read-only 审计 agent，对每条 HIGH/MED 发现再派
对抗验证 agent（默认立场「这条是错的」，逼其举反例，无法证伪且能复述触发路径才判 CONFIRMED）。规模：
4 个 finder，0 error，约 27 万 subagent tokens、52 次工具调用（git diff + 读源码 + 交叉核对本地 libobs）。

四个视角：
1. **render-correctness**：矩阵栈平衡、`goto` 是否跨初始化、90/270 包围盒铺满、mirror+rotation 组合、
   方向与 OBS 一致性、R0 零回归、snap-to-fill/Below-label/诊断日志未被破坏、缠绕/剔除。
2. **concurrency-lifecycle**：矩阵操作是否纯 GPU 无回调；群组 `inc/dec_showing` 遍历场景树 + 第三方 hide/show
   回调是否只在主线程锁外；`mutate_cell_visual` push_back 后有无悬垂引用。
3. **data-serialization**：round-trip 对称、缺键回落 R0/Inherit（旧配置零回归）、resolve 三级链、
   `rotation_from_degrees` 的 `((deg%360)+360)%360` 对负/大 delta 的正确性、`effective_visuals_` 索引守卫。
4. **ui-and-groups-semantics**：菜单新方法是否走既有安全范式、继承回退边界、`toggle_cell_full_mirror`
   语义、非 cell hit 守卫、群组标为 `"source"` 的下游影响是否可接受。

结果：视角 2/3/4 对抗后**无发现**；视角 1 报 1 条 LOW。

## 修复清单

| 级别 | 文件 | 问题 | 处置 |
|---|---|---|---|
| LOW | `src/amv-instance-core-draw.cpp` `[fill]` 诊断日志 | 拟合用的 `srcAspect` 改为 `fitW/fitH`（90/270 交换）后，日志仍以原始 `src=%ux%u` 配这个已交换的比值，二者不自洽（如 `1920x1080 (0.5625)`），误导读日志者 | 日志 `src=WxH` 配回原始比值 `srcW/srcH`，并补 `rot=` 与 `fit=`（旋转角 + 实际拟合比值），使旋转格的填充调试也可读。已并入 `feat` 提交 |

## 观察项（已分析，为何不改 / 为何安全）

- **矩阵栈平衡**：`rotated`（`rot != R0`）为真才 `gs_matrix_push`，`gs_matrix_pop` 亦仅在 `rotated` 时执行，
  二者同条件、区间内无早返回（PGM / 普通源 / 空渲染都线性走完），栈严格平衡。`R0` 完全不压矩阵。
- **`goto render_no_signal` 不跨初始化**：唯一的 `goto`（`srcW||srcH == 0`）位于 `rot/fitW/fitH/dispW/dispH`
  等带初始化变量**声明之前**，label 在 `else` 分支外层，是「跳出作用域」而非「跳入越过初始化」，合法。
- **R0 零回归**：`R0` 时 `fitW/fitH == srcW/srcH`、`dispW/dispH == srcW/srcH`，`oL/oR/oT/oB` 与改动前逐位相同，
  且不压矩阵——渲染路径与旧版逐位等价。
- **方向对齐 OBS**：`gs_matrix_rotaa4f(0,0,1,+rad)` 与 OBS `obs-scene.c` 的 `matrix4_rotate_aa4f(...,0,0,1,RAD(rot))`
  同一约定，正角度 = 屏幕（Y 向下）顺时针；已真机验证「顺 90」与 OBS「变换→顺时针旋转 90 度」一致。
- **mirror × rotation 组合**：镜像仍是 `gs_ortho` 展示空间边界翻转，旋转是 model 矩阵；作用于不同变换级、
  确定性叠加，`det` 组合不影响可见性（OBS 全程 `GS_NEITHER` cull）。
- **群组 `inc/dec_showing`**：群组 = SCENE 型源，其 `showing` 增减遍历子源、可 fire 第三方 hide/show 回调——
  但这与既有「场景」型 cell 走**同一条** `obs_get_source_by_name + inc_showing` 路径、同在主线程锁外，
  未引入群组特有的新崩溃面（嵌套群组/含外部源同样沿用场景语义）。群组标为 `"source"` 故不参与场景点击切换 /
  PGM 嵌套高亮，是符合预期的取舍（用户要的是显示群组合成画面，而非把它当可切换场景）。
- **序列化**：`rotation`/`rotationMode` 与 mirror 逐一并列、同构 round-trip；缺键回落 `R0`/`Inherit`，旧配置无损。
- **旋转 UI 仅 cell 级（继承 UI 有意暂缺）**：按需求决策，旋转只在右键菜单暴露 cell 级操作；右键子菜单里的
  「继承（跟随实例/全局）」**置灰不可点**（`rInherit->setEnabled(false)`）——没有实例/全局旋转编辑入口、无可继承
  对象，复位由「无旋转（0°）」承担。**数据模型的三级继承脚手架全部保留（未删）**，便于将来补齐继承 UI；
  清单与重启步骤见下节「未来工作」。

## 验证步骤

真机（用户已验收）：
1. 横向 cell merge 加竖条源 → 右键「旋转→顺/逆 90」→ 旋转且按横向充满、不再被压小。
2. 顺 90 与 OBS 视觉一致；连点 4 次回原位；逆 90 == 顺 90×3。
3. 顺/逆/180/无旋转(0°) 即时生效；「继承（跟随实例/全局）」置灰不可点。
4. PGM 格旋转正确；镜像 × 旋转任意组合确定、无闪烁；R0 格与改动前一致。
5. 设置后存盘、重开 OBS 无损；旧配置默认无旋转无镜像。
6. source picker「源」标签可搜到并添加群组，单元格显示其合成画面。

回归面：`R0` + 无镜像的普通格必须与改动前逐位一致（已从代码路径论证）。

## 未来工作：重新启用旋转的「继承」UI（脚手架已就位，勿重复造轮子）

旋转当前只暴露 cell 级右键菜单；实例/全局级 UI 有意暂缺（右键「继承」项置灰）。但**三级继承的底层脚手架已完整
保留**，将来若要补齐，只需接 UI，不必改数据层。已就位的部分：

- **数据模型**（`src/multiview-instance.hpp`）：`GlobalVisualSettings.rotation`、
  `InstanceVisualSettings.{rotationMode,rotation}`、`CellVisualSettings.{rotationMode,rotation}`、
  `EffectiveCellVisualSettings.rotation`，与 mirror 逐一并列。
- **序列化**（`src/multiview-instance-serialize-visual.cpp` + `-serialize.hpp`）：三容器 `to/from_obs_data`
  读写 `rotation`/`rotationMode`；`rotation_angle_to/from_str`。旧配置缺键回落 `R0`/`Inherit`。
- **解析链**（`resolve_effective_visual_settings`）：旋转已走 cell→instance→global 三级 override，与 mirror 同构。
- **渲染**（`amv-instance-core-draw.cpp`）：直接消费 `effective_visuals_[i].rotation`，与继承层级无关，无需改动。
- **cell 级 mutator**（`src/multiview-window-context-menu.cpp`）：`set_cell_rotation_inherit(row,col)`
  **已实现但当前未调用**（保留，勿删）。

**重启继承 UI 的最小步骤：**
1. **对话框**（`src/cell-display-settings-dialog.{hpp,cpp}`）：仿 `create_mirror_group()` 加一个旋转组
   （继承下拉 + 旋转控件），并在 `set/get_{global,instance,cell}_settings` 里读写 `rotation`/`rotationMode`；
   `on_reset` 走 `to/from_obs_data` 自动覆盖。
2. **右键菜单**（`src/multiview-window-context-menu.cpp` 旋转子菜单）：去掉 `rInherit->setEnabled(false)`，
   恢复 `setCheckable(true)` + `setChecked(cvs->rotationMode == Inherit)` + `connect(... set_cell_rotation_inherit ...)`
   （该 mutator 代码仍在，直接接回）。

数据层、序列化、解析、渲染均已就绪，**无需再动**。

## 备注

- 诊断日志修复后仅 Debug 已重编验证；部署包（RelWithDebInfo）为纯诊断字符串差异、功能等价，
  待下次 OBS 关闭时随常规构建一并重新部署即可。
- 版本号显示问题（曾显示 `1.0.0-rc.11`）与本特性无关：根因是 `build_x64` 停留在早期配置，`PLUGIN_VERSION`
  于 CMake 配置阶段烘焙；重跑配置后已为 `1.1.0`，仓库 `buildspec.json` 无需改动。
