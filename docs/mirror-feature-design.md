# 每格镜像（水平 / 垂直翻转）设计文档

## 动机

Display Settings（`CellDisplaySettingsDialog`）需要为每格信号画面提供**水平镜像（左右翻转）**与
**垂直镜像（上下翻转）**。典型诉求：某路摄像头 / NDI 物理左右反了、自拍监看要照镜子效果等——都是
**单源级**的翻转，与 OBS 原生「右键源 → 变换 → 水平/垂直翻转」语义一致。

## 约束（对照 AGENTS.md §0）

- 绝不炸 / 绝不阻塞 OBS：本功能是**纯 GPU 投影状态**改动，零锁、零 `obs_source` 引用计数增减、
  不在渲染线程做任何可能阻塞的调用。
- 隔离：镜像是逐格视觉设置，走既有三级继承，天然按格 / 按实例隔离。

## 方案

### 1. GPU 层技术：交换 `gs_ortho` 边界

信号画面在 `amv-instance-core-draw.cpp` 的「Render into video rect」处渲染：先 `startRegion()`
建立 `gs_ortho(0, srcW, 0, srcH, …)`，再 `obs_render_main_texture()`（PGM）或
`obs_source_video_render(src)`。

镜像只需把喂给 `gs_ortho` 的边界按开关交换：
- 水平镜像：`left/right` 由 `0,srcW` → `srcW,0`。
- 垂直镜像：`top/bottom` 由 `0,srcH` → `srcH,0`。

数学依据（核对 OBS `device_ortho`，`libobs-d3d11/d3d11-subsystem.cpp`）：投影矩阵
`x.x = 2/(right-left)`，交换 `left/right` 使其变负——源 `x=0` 映射到屏幕右、`x=srcW` 映射到屏幕左，
即水平镜像；垂直同理。

**为什么不用 `gs_matrix_scale3f(-1,1,1)`**：负缩放会把几何推到负轴、需补偿 translate，且污染 source
自己 push 的 model 矩阵。**为什么不用 texrender + `GS_FLIP`**：多一张离屏纹理 + 一次拷贝，无谓开销。
ortho 交换是零额外资源、与既有 viewport/translate 栈正交的最小改动。

**与 OBS 内部机制的关系**：OBS 场景项翻转本质是 `scale.x/scale.y` 取负写入 `draw_transform` 矩阵
（`libobs/obs-scene.c`），并配 TL 位置补偿——那是「持久化到 sceneitem」的场景。我们只在**渲染时**翻转、
不改任何 sceneitem，故无需 TL 补偿，直接翻投影即可。

### 2. 关键安全性核对

- **背面剔除**：负行列式反转三角缠绕，但 OBS 所有渲染路径强制 `gs_set_cull_mode(GS_NEITHER)`
  （`obs-video.c` / `obs-display.c`），反向缠绕永不被剔除——这是「裸投影翻转即可、无需修缠绕」的根本原因。
- **async 源双翻转**：async 视频源内部可能已按 `frame->flip` 施加 `GS_FLIP_V`（`obs-source.c`）。
  那是**纹理 UV 层**翻转，我们做的是**投影矩阵层**翻转，二者作用于不同变换级、确定性叠加，不会互相抵消。
  用户看到的就是「画面上下颠倒」——正是垂直镜像应有结果。水平/垂直镜像都可安全提供，无需对 async / PGM 特判。
- **PGM vs 普通源**：`obs_render_main_texture()` 与 `obs_source_video_render()` 在同一
  `startRegion/endRegion` 内、都渲染进当前投影，随 ortho 交换同步翻转，无需 `isPgm` 分支。
- **零回归**：默认两个开关皆 false，走 `oL=0,oR=srcW,oT=0,oB=srcH`，与改动前逐位相同。

### 3. 镜像什么 / 保持什么

**只镜像信号画面**（那一个 render region 内的 PGM/源画面）。以下全部**保持不翻转**，因为它们各自 open
自己未翻转的 region，且必须保持可读 / 正确朝向：Label 文字、Overlay 图片、Safe-area 安全框、VU 表、
Status band、PGM/PRVW 高亮边框、背景色 / 背景图。Signal-anchor 的 overlay / safe-area 继续用**同一未翻转
vr 矩形**定位——正是期望行为。

## 数据模型

新增 `MirrorSettings { bool horizontal; bool vertical; }` 视觉组，走完整三级继承
（Global → Instance → Cell），与现有 Overlay 组完全同构：

- `multiview-instance.hpp`：`MirrorSettings` 结构体 + `to/from_obs_data`；加入
  `GlobalVisualSettings` / `InstanceVisualSettings`（+`mirrorMode`）/ `CellVisualSettings`（+`mirrorMode`）/
  `EffectiveCellVisualSettings`。
- `multiview-instance-serialize-visual.cpp`：`MirrorSettings` 序列化；Global/Instance/Cell 容器读写；
  `resolve_effective_visual_settings` 三级链（cell override → instance override → global）。
- 持久化键：视觉组子对象 `"mirror"`，字段 `"horizontal"` / `"vertical"`；继承字段 `"mirrorMode"`。
- 前向兼容：旧 config 缺字段默认 false（= 当前无镜像）。

## UI

### Cell Display Settings 对话框

新增 `create_mirror_group()`，含继承下拉（非 Global）+ 两个 checkbox（水平翻转 / 垂直翻转），
接 `HOOK_CHECK`、`toggle_group`、set/get × 3（Global/Instance/Cell）。Copy/Paste/Reset 经
`to/from_obs_data` 自动生效。

### 右键上下文菜单（`multiview-window-context-menu.cpp`）

在 cell-hit 段新增「镜像」子菜单，含两个可勾选项（水平翻转 / 垂直翻转）。**作用于悬停格**：勾选状态反映
该格当前生效镜像值；点击即把该格 `CellVisualSettings.mirror.{h,v}` 设为切换值、`mirrorMode = Override`，
`config_->save()` + `refresh_visual_settings()` 重绘。与 OBS 原生 per-source 翻转语义一致。

## Locale（en-US.ini 基准 + zh-CN.ini，key 集合必须一致）

```
AMVPlugin.Visual.Mirror.Title       = "Mirror"          / "镜像"
AMVPlugin.Visual.Mirror.Horizontal  = "Flip Horizontal" / "水平翻转"
AMVPlugin.Visual.Mirror.Vertical    = "Flip Vertical"   / "垂直翻转"
```

复用现有 `AMVPlugin.Visual.Inheritance`、`AMVPlugin.Visual.Common.Visibility`。

## 实现顺序

数据模型 → 序列化/resolve → 渲染接入 → 对话框 → 右键菜单 → locale → clang-format → build Debug+Rel → deploy。
