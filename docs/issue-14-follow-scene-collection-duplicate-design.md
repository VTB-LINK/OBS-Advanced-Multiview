# issue #14 — 复制场景集合时让 AMV 实例跟随（设计）

## 动机

AMV 配置**按场景集合分文件**存：`<plugin_config>/obs-advanced-multiview/settings-<集合名>.json`
（见 `ConfigManager::get_config_file_path`）。用户在 OBS 里「复制场景集合」后，新集合里 AMV 是空的——
用户得手动重建全部实例。issue #14 要求：复制时让 AMV 实例跟着搬到新集合。

## OBS 复制场景集合做了什么（核对源码）

`OBSBasic::SetupDuplicateSceneCollection`（`frontend/widgets/OBSBasic_SceneCollections.cpp`）：

1. 新建一个集合（新文件）；
2. `SaveProjectNow()` 存当前；
3. **把当前集合文件整份拷贝**到新集合文件；
4. 只重新生成源的 UUID——**场景/源的名字保持不变**；
5. `ActivateSceneCollection(new)` → 切到新集合，发 `SCENE_COLLECTION_CHANGING` → `SCENE_COLLECTION_CHANGED`。

关键：**没有专门的"已复制"前端事件**，复制与普通切换发的是同一个 `CHANGED`。

## 有利条件

- AMV 的 cell 按**名字**引用场景/源（`CellAssignment.type` = "scene"/"source"，`name` = 名字）。复制后名字
  都在，所以把旧实例原样搬过去能正确解析。
- `on_scene_collection_changed` 在 `save()`（存旧）之后、`load()`（读新）之前，内存里还握着旧集合的全部
  配置（instances + global + presets）。

## 检测：判定"这是一次复制/兼容集合"

在 `SCENE_COLLECTION_CHANGED`，三条**全部满足**才判定为可跟随：

1. **新集合是刚创建的**：维护一份「已知集合名」快照 `known_collections_`（启动 load 时初始化 =
   `obs_frontend_get_scene_collections()`；每次 `on_scene_collection_changed` 处理**完**再刷新一次）。
   若新集合名**不在**上一份快照里 ⇒ 它是自上次以来新出现的 = 刚被创建。**绝不**在 LIST_CHANGED 时刷新
   快照（否则新集合会在 CHANGED 前就被记入，判断失效）。
   → 这条保证：切到**任何早已存在的集合**（哪怕场景名一模一样、内容不同）都**不会**触发。
2. **旧集合有 AMV 实例**（否则没什么可搬）。
3. **新集合按名字能解析到**旧实例引用的场景/源：对旧实例每个 type ∈ {scene, source} 的 assignment，
   `obs_get_source_by_name(name)` 在新集合里非空。全部命中才算兼容。（pgm/prvw/external cell 不按集合
   场景名引用，视为天然兼容。）这条把"新建空集合（只有默认 Scene）"挡在门外。

**残留误判**（已极小化）：用户**手动**新建集合又刚好把场景命名得与旧集合完全一致——会弹一次，点「不用」即可。
真正的复制操作必定命中；无关的既有集合永不打扰。

## 行为：弹窗询问（用户选定）

命中检测后，弹一个非模态确认框。**文案一律无人称**（不含"你/我/您/它"等指代）：

> **复制多视图配置到新场景集合？**
> 检测到场景集合 “<旧>” 被复制为 “<新>”。是否把已有的 N 个多视图实例及相关设置一并复制到新集合？
> ⚠ 此提示仅在复制场景集合时出现这一次；点击「取消」后将不会自动复制 AMV 配置，只能手动重建。
> [复制] [取消]

- **复制**：把旧集合的**整份 AMV 配置**搬进新集合——即 **globalSettings**（默认 gutter、帧率重解析、
  多视图窗口帧率分频、全局视觉默认、全局场景点击、信号丢失默认、NDI 读回双缓冲、详细日志）
  + **全部 instances**（各分配**新 UUID**；每实例自带的 NDI/Spout 外部输出、视觉/信号丢失覆盖随之带过来）
  + **layoutPresets**。写 `settings-<新>.json`；`load()` 让运行时接管；刷新 manager 列表、重建输出核心
  （复用现有 `on_scene_collection_changed` 尾部的 orphan 清理 / 输出重建）。
  - **为何整份**：全局设置也是**按集合分文件存**的，且实例继承全局（gutter/视觉/场景点击）。只搬实例、
    全局用默认，会让继承全局的实例在新集合里变样、不保真。整份复制才与 OBS "复制整个场景集合"语义对齐。
- **取消**：保持空（现状行为）。
- 每次复制事件只弹一次；`CHANGED` 事件做去抖合并，避免单次复制弹多个窗。命中判定见上"检测"：
  切到既有集合永不弹，取消后也不再为该集合追问。

## 姊妹需求：场景集合重命名 → 同步改配置文件名

场景集合被**重命名**（A→B）时，AMV 应把配置文件 `settings-A.json` **改名**为 `settings-B.json`，
而不是留下孤儿文件、新名加载空配置。

- 监听 `OBS_FRONTEND_EVENT_SCENE_COLLECTION_RENAMED`（若该事件不带旧名，则用 `known_collections_` 快照
  与当前列表求差推断"消失的旧名 + 新增的新名"）。
- 处理：若 `settings-<旧>.json` 存在且 `settings-<新>.json` 不存在 → `os_rename` 之；随后按新名 `load()`。
- 与"复制检测"的先后：重命名会先于/独立于 `CHANGED` 到达；重命名把文件搬到新名后，`CHANGED` 到新名时
  `settings-<新>.json` 已存在 → 不触发复制弹窗（正确：重命名不是复制）。
- 失败安全降级：改名失败则退回现状（新名空配置 + 旧文件残留），只记日志，不崩。

## 实施要点 / 落点

- `config-manager.hpp/.cpp`：
  - 新增 `known_collections_`（`std::set<std::string>`）+ 初始化/刷新。
  - `on_scene_collection_changed` 增加检测：算出 `duplicateSource`（旧集合名 + 旧配置快照）与命中与否，
    通过一个回调/返回值把"待询问"交给 UI 层（ConfigManager 不弹 Qt 窗）。
  - 新增 `seed_current_collection_from(const std::vector<MultiviewInstance>&, const GlobalSettings&,
    const std::vector<LayoutPreset>&)`：写入并保存当前集合配置（实例重分配 UUID）。
- `plugin-main.cpp`：`on_frontend_event` 的 `SCENE_COLLECTION_CHANGED` 分支里，拿到 ConfigManager 给出的
  "可跟随"信号后，在**主线程**弹 Qt 确认框（父窗口 = OBS 主窗），用户确认则调用 seed + 重载 + 刷新。
  非模态，绝不阻塞。
- locale：确认框标题/正文/按钮 key（en-US + zh-CN）。

## 稳定性（对照 AGENTS.md）

- 全程在**主线程**（前端事件回调 + Qt 弹窗）；不碰渲染线程、不持 source_mutex_、不在信号线程弹窗。
- 检测只读（枚举名字、查 `obs_get_source_by_name`）；不改 OBS 侧任何状态。
- 复制失败/取消都安全降级为"空集合"（现状）。
- 旧集合配置只读快照；新集合 seed 走既有原子保存（tmp+rename）。
- 重命名场景集合也会走同一条（结果正确：实例出现在新名下），代价是残留一个 `settings-<旧名>.json`；
  可选地日后用 `SCENE_COLLECTION_RENAMED` 清理，本期不做。

## 验证

- 复制集合 → 弹窗 → 复制 → 新集合出现同样的实例，cell 正确解析到同名源；重开 OBS 后仍在。
- 切到既有的、场景名相同但内容不同的集合 → **不弹**。
- 新建空集合 → **不弹**。
- 取消复制 → 新集合保持空；再不打扰。
