# Phase 1 Development Breakdown

> 本文件用于说明 OBS Advanced Multiview 第一阶段开发范围与任务拆分。  
> 第一阶段对应 `ROADMAP.md` 中的 Milestone 0 到 Milestone 3。  
> 本文件是 `ROADMAP.md` 的补充说明，不替代 `ROADMAP.md`。

---

## 1. 第一阶段范围

第一阶段开发范围为：

```text
Phase 1 = Milestone 0 + Milestone 1 + Milestone 2 + Milestone 3
```

也就是：

- Milestone 0：仓库整理与基础骨架
- Milestone 1：配置系统与管理 / 设置 Dialog
- Milestone 2：布局引擎与编辑网格
- Milestone 3：MultiviewWindow 与 OBS 内部信号渲染

完成 Milestone 0 到 Milestone 3 后，项目应形成第一个真正可用的 MVP 闭环。

---

## 2. 为什么 Phase 1 不包含 Milestone 4

Milestone 4 包含视觉参数与辅助功能，例如：

- 底色
- 底图
- 前景 overlay
- 安全区示意图，例如 EBU R95
- VU meter 开关
- 标签名显示模式

这些功能非常重要，但不建议放入第一阶段。

原因：

- 它们会显著扩大第一阶段开发面。
- 它们依赖 Milestone 3 中的 MultiviewWindow 与 MultiviewRenderer 基础能力。
- VU meter、overlay、安全区和标签显示模式会牵涉配置模型、Renderer、UI 设置入口、保存语义与性能策略。
- 第一阶段应优先完成可用、稳定、可保存、可恢复的 OBS 内部信号 Multiview 闭环。

因此建议：

```text
Phase 1：Milestone 0 ~ Milestone 3
Phase 2：Milestone 4
Phase 3：Milestone 5 ~ Milestone 6
Phase 4：Milestone 7 ~ Milestone 8
```

---

## 3. Phase 1 总体验收闭环

第一阶段完成后，用户应能够完成以下流程：

```text
用户打开 OBS
  -> 工具
  -> OBS Advanced Multiview
  -> 管理 / 设置 Dialog
  -> 新建 Multiview 实例
  -> 编辑 4x4 / 5x5 / 2+18 或自定义 span 布局
  -> 打开 MultiviewWindow
  -> 右键 cell
  -> 选择 PGM / PRVW / Scene / Source
  -> 画面显示出来
  -> 保存布局
  -> 保存网格信号
  -> 关闭 OBS
  -> 重新打开 OBS
  -> 配置和信号绑定恢复
```

并且必须满足：

- OBS source 被删除或失效时插件不崩溃。
- 窗口关闭不删除 Multiview 实例。
- 重复打开同一 Multiview 实例时聚焦已有窗口。
- gutter / border hit-test 正确。
- 布局修改后已打开的当前 MultiviewWindow 动态更新，无需重启 OBS。
- 添加 / 更换 / 清空 cell 后当前 MultiviewWindow 动态更新，无需重启 OBS。
- OBS 退出时安全关闭所有窗口并释放引用。
- 配置保存失败时不得破坏已有配置。

---

## 4. Milestone 0 拆分

Milestone 0 的目标是让插件基础工程“站起来”。

### 0.1 插件入口

目标：

- 确认插件能被 OBS 加载。
- 整理 plugin entry point。
- 保留 obs-plugintemplate 风格，不做过度工程化。

验收：

- OBS 启动后能加载插件。
- 插件加载失败时有可诊断日志。

---

### 0.2 工具菜单入口

目标：

- 在 OBS 顶部菜单“工具”下添加 OBS Advanced Multiview 入口。

验收：

- 用户可通过 `工具 -> OBS Advanced Multiview` 打开入口。
- 菜单项重复注册、重复加载、卸载时不产生异常。

---

### 0.3 空管理 / 设置 Dialog

目标：

- 点击工具菜单后打开基础管理 / 设置 Dialog。
- Dialog 暂不需要完整功能。

验收：

- Dialog 可以打开和关闭。
- 重复打开时行为明确。
- OBS 退出时 Dialog 能安全关闭。

---

### 0.4 plugin_config 路径

目标：

- 插件能够定位并创建自身配置目录。

路径：

```text
plugin_config/obs-advanced-multiview/
```

验收：

- 安装版与便携版均依赖 OBS 的 plugin_config 机制。
- 不写入插件安装目录。
- 目录创建失败时有错误处理，不影响 OBS 主流程。

---

### 0.5 基础日志 / 安全卸载

目标：

- 建立基础日志约定。
- OBS 退出或插件卸载时安全释放资源。

验收：

- 不遗留窗口。
- 不遗留后台任务。
- 不访问已释放对象。
- 不影响 OBS 主线程、主输出或其他插件。

---

## 5. Milestone 1 拆分

Milestone 1 的目标是让插件能管理 Multiview 实例和配置。

### 1.1 JSON schema

目标：

- 定义配置 JSON 结构。
- 包含 `configVersion`。
- 预留 `Layout Preset` 数据结构。

至少包含：

- 全局设置。
- Multiview 实例列表。
- Layout Preset 列表。
- 每个 Multiview 实例的 UUID。
- 每个 Multiview 实例的名称。
- 每个 Multiview 实例的 layout 数据。
- 每个 Multiview 实例的 cell assignment 数据。
- 每个 Layout Preset 的 UUID、名称、rows、columns、span、gutter / border。

验收：

- 可以读取不存在的配置并生成默认配置。
- 可以读取已有配置。
- 配置字段缺失时能安全 fallback。
- JSON 损坏时不得导致 OBS 崩溃。

---

### 1.2 原子化保存

目标：

- 避免写入半截 JSON 导致配置损坏。

建议：

```text
write temporary file
flush / close
rename old file to backup if needed
rename temporary file to target file
```

验收：

- 保存失败时保留旧配置。
- 保存失败时 UI 或日志给出错误。
- 不因为保存失败影响 OBS 主流程。

---

### 1.3 场景集合名配置文件

目标：

- 配置按当前 OBS 场景集合名分文件保存。

建议文件名：

```text
settings-<场景集合名>.json
```

验收：

- 文件名经过非法字符清洗。
- 场景集合名变化时使用新名字另存。
- 旧文件延迟删除。
- 删除失败不影响插件运行。

---

### 1.4 Multiview 实例列表

目标：

- 管理 / 设置 Dialog 左侧显示 Multiview 实例列表。

验收：

- 能显示已有实例。
- 能显示空状态。
- 能选择实例并在右侧显示详情区域。

---

### 1.5 新建 / 重命名

目标：

- 支持新建 Multiview 实例。
- 支持重命名 Multiview 实例。

验收：

- 新建实例生成 UUID。
- 重命名后能保存。
- 重名、空名等边界有合理处理。
- 重启 OBS 后名称恢复。

---

### 1.6 克隆 / 删除

目标：

- 支持克隆整个 Multiview 实例。
- 支持删除整个 Multiview 实例。

验收：

- 克隆生成新的 UUID。
- 克隆语义是克隆整个 Multiview 实例。
- 删除操作只在管理 / 设置页面中出现。
- 右键菜单中不出现克隆和删除整个 Multiview 实例。
- 删除已打开实例时行为需要安全处理。

---

### 1.7 打开 / 聚焦窗口占位

目标：

- 管理 / 设置 Dialog 中支持打开 / 聚焦实例窗口。

验收：

- 未打开时显示或执行“打开”。
- 已打开时显示或执行“聚焦”。
- 同一 Multiview 实例同一时间只允许一个可编辑窗口。
- 重复打开同一实例不创建第二个同 UUID 窗口。

---

### 1.8 Layout Preset 数据结构预留

目标：

- 预留独立 Layout Preset 数据结构。
- 第一阶段仍以 Multiview 实例保存布局为主。

Layout Preset 只保存：

- 名称。
- UUID。
- rows。
- columns。
- span 合并结构。
- gutter / border 厚度。
- 与布局相关的默认显示参数。

Layout Preset 不保存：

- PGM / PRVW / Scene / Source 绑定。
- cell assignments。
- 外部流配置。

验收：

- JSON 结构允许后续加入和读取 Layout Preset。
- 不要求第一阶段完成完整 Layout Preset 管理 UI。

---

## 6. Milestone 2 拆分

Milestone 2 的目标是实现可验证的布局能力。

### 2.1 LayoutEngine 数据结构

目标：

- 建立基础网格 + span 的数据结构。

验收：

- 能表达 rows / columns。
- 能表达 cell。
- 能表达 span。
- 能表达 gutter / border。

---

### 2.2 rows / columns 校验

目标：

- 支持 1x1 到 10x10。

验收：

- rows 范围为 1 到 10。
- columns 范围为 1 到 10。
- 超出范围时拒绝或安全回退。

---

### 2.3 span 校验

目标：

- 支持多个基础 cell 合并为 span。

验收：

- span 不得小于 1x1。
- span 不得超出基础网格边界。
- span 不得重叠。
- 无效 span 不得导致崩溃。

---

### 2.4 gutter / border

目标：

- 支持 gutter / border 厚度。
- gutter / border 是 cell 上下左右之间的间距 / 边缘，不仅是左右缝隙。

验收：

- 范围为 0 到 50 px。
- 上下左右间距 / 边缘均被计算。
- hit-test 能识别 gutter / border。

---

### 2.5 rect 计算

目标：

- 根据当前窗口尺寸、rows、columns、span、gutter / border 计算每个 cell / region 的显示 rect。

验收：

- 规则网格正确。
- span 后 rect 正确。
- 窗口尺寸变化后 rect 正确更新。
- 不出现负尺寸或越界。

---

### 2.6 video rect 计算

目标：

- 在 cell / region 内计算视频显示区域。

验收：

- 保持源比例。
- 非等比区域使用 letterbox / pillarbox。
- 不做铺满拉伸。
- 不做裁切。

---

### 2.7 hit-test

目标：

- 鼠标位置映射到已有信号 cell、空 cell、gutter / border。

验收：

- 能区分已有信号 cell。
- 能区分空 cell。
- 能区分 gutter / border。
- gutter / border 覆盖 cell 上下左右之间的间距 / 边缘。

---

### 2.8 编辑网格 UI

目标：

- 在管理 / 设置 Dialog 右侧详情区域提供编辑网格页面。

验收：

- 左侧仍是导航列表 / 实例列表。
- 右侧渲染当前 Multiview 实例的编辑网格内容。
- 可调整 rows / columns / span / gutter / border。

---

### 2.9 保存布局

目标：

- 保存当前 Multiview 实例布局。

验收：

- 保存 rows。
- 保存 columns。
- 保存 span。
- 保存 gutter / border。
- 重启 OBS 后恢复布局。

---

### 2.10 动态应用布局到已打开窗口

目标：

- 修改布局后，已打开的当前 MultiviewWindow 动态更新，无需重启 OBS。

验收：

- rows / columns 修改后窗口更新。
- span 修改后窗口更新。
- gutter / border 修改后窗口更新。
- 无效修改不会破坏当前已渲染窗口。

---

## 7. Milestone 3 拆分

Milestone 3 的目标是实现第一阶段真正可用的 MultiviewWindow 与 OBS 内部信号渲染。

### 3.1 MultiviewWindow 空窗口

目标：

- 支持打开独立 MultiviewWindow。

验收：

- 从管理 / 设置 Dialog 打开窗口。
- 重复打开同一实例时聚焦已有窗口。
- 关闭窗口不删除 Multiview 实例。

---

### 3.2 MultiviewRenderer 绘制 cell / gutter / border

目标：

- MultiviewRenderer 能绘制基础布局。

验收：

- 绘制 cell 背景。
- 绘制 gutter / border。
- 绘制空 cell。
- 绘制 hover / 选中态如需要。
- 不依赖具体信号也能显示布局。

---

### 3.3 右键菜单

目标：

- 根据 hit-test 结果显示不同右键菜单。

已有信号 cell：

```text
全屏
窗口置顶
---
更换 / 编辑信号源
清空此格
编辑网格
保存网格信号
---
全局设置
关闭
```

空 cell：

```text
全屏
窗口置顶
---
添加 / 选择信号源
编辑网格
保存网格信号
---
全局设置
关闭
```

gutter / 缝隙：

```text
全屏
窗口置顶
---
编辑网格
保存网格信号
---
全局设置
关闭
```

验收：

- 右键菜单中不出现“打开 / 聚焦窗口”。
- 右键菜单中不出现“克隆”。
- 右键菜单中不出现“删除”整个 Multiview 实例。
- 右键菜单中不出现“复制网格 UUID”。

---

### 3.4 Source Picker 列表 + 搜索

目标：

- 实现第一版 Source Picker。

验收：

- 使用列表 + 搜索。
- 不做 OBS 32 风格卡片预览。
- 不做实时缩略图网格。
- 支持选择 PGM。
- 支持选择 PRVW。
- 支持选择 Scene。
- 支持选择 Source。

---

### 3.5 PGM / PRVW provider

目标：

- 支持 PGM / PRVW 作为 cell 信号。

验收：

- cell 可绑定 PGM。
- cell 可绑定 PRVW。
- PGM / PRVW 渲染异常时不崩溃。

---

### 3.6 Scene / Source provider

目标：

- 支持 Scene / Source 作为 cell 信号。

验收：

- cell 可绑定任意 Scene。
- cell 可绑定任意 Source。
- Source 删除或失效时插件不崩溃。
- Source 重命名时不应破坏插件运行。

---

### 3.7 绘制 OBS 内部信号

目标：

- MultiviewRenderer 能绘制 PGM / PRVW / Scene / Source。

验收：

- 信号显示在正确 cell。
- 信号保持源比例。
- 使用 letterbox / pillarbox。
- 不做铺满拉伸或裁切。
- 多 cell 同时显示时不影响 OBS 主输出。

---

### 3.8 添加 / 更换 / 清空 cell

目标：

- 支持 cell assignment 修改。

验收：

- 空 cell 可添加 / 选择信号源。
- 已有信号 cell 可更换 / 编辑信号源。
- 已有信号 cell 可清空。
- 空 cell 不显示“清空此格”。
- gutter / border 不显示 cell 级操作。

---

### 3.9 保存网格信号

目标：

- 保存当前 Multiview 实例的 cell assignments。

验收：

- 保存 PGM / PRVW / Scene / Source 绑定。
- 保存空 cell 状态。
- 重启 OBS 后恢复 cell 绑定。
- 管理 / 设置 Dialog 中也能看到信号状态，并提供保存网格信号入口。

---

### 3.10 动态应用 cell assignment

目标：

- 添加 / 更换 / 清空 cell 后，当前 MultiviewWindow 动态更新，无需重启 OBS。

验收：

- 添加信号后立即显示。
- 更换信号后立即更新。
- 清空 cell 后立即显示为空 cell。
- 保存网格信号不应成为画面更新的前置条件。

---

### 3.11 source 删除 / 失效保护

目标：

- OBS 内部源删除或失效时不崩溃。

验收：

- 被引用 Source 删除时，cell 进入安全状态。
- 被引用 Scene 删除时，cell 进入安全状态。
- PGM / PRVW 不可用时，cell 进入安全状态。
- 插件不访问已释放对象。
- 插件不影响 OBS 主线程、主输出或其他插件。

---

### 3.12 全屏 / 置顶 / 关闭

目标：

- 支持 MultiviewWindow 窗口级操作。

验收：

- 支持全屏。
- 支持窗口置顶。
- 支持关闭窗口。
- 关闭窗口不删除 Multiview 实例。
- OBS 退出时窗口安全关闭。

---

### 3.13 Qt 窗口句柄与独立渲染管线实现结论

目标：

- 结合 OBS 31.1.1 到 32.1 的实际 API 与仓库结构确认实现方式。

验收：

- 不在未验证 API 的情况下假设具体渲染上下文方案。
- 明确 MultiviewWindow 与 MultiviewRenderer 的实际边界。
- 明确哪些内容需要放到 Milestone 8 做性能、稳定性与回归验证。

---

## 8. Phase 1 不包含内容

第一阶段明确不包含：

- 完整 Layout Preset 管理 UI。
- OBS 32 风格 source card 预览。
- 实时 source 缩略图网格。
- NDI。
- Spout。
- RTMP。
- HLS / M3U8。
- FLV。
- SRT。
- WebRTC。
- VU meter。
- overlay。
- 安全区示意图。
- 底色。
- 底图。
- fallback 视频 / 图片。
- 完整 Signal Lost 策略。
- 外部流 backoff 重试。
- 外部流立即重连。
- artifacts 打包完善。

Phase 2.5 已完成；这些内容应在 Phase 3（M5~M6）、Phase 4（M7~M8）时再详细规划，详见 [TERMINOLOGY.md](TERMINOLOGY.md) 与 [ROADMAP.md](ROADMAP.md)。

---

## 9. Phase 1 完成定义

Phase 1 完成时，OBS Advanced Multiview 应至少是一个稳定的 OBS 内部信号 Multiview 插件。

最低完成定义：

- 能创建 Multiview 实例。
- 能编辑基础网格 + span 布局。
- 能保存布局。
- 能打开独立 MultiviewWindow。
- 能在 cell 中添加 PGM / PRVW / Scene / Source。
- 能更换 / 清空 cell。
- 能保存网格信号。
- 能重启后恢复布局与网格信号。
- 能全屏。
- 能窗口置顶。
- 能关闭窗口。
- 能处理 OBS source 删除或失效，不崩溃。
- 能在 OBS 退出时安全释放。
- 布局和 cell assignment 修改动态生效，无需重启 OBS。
- 配置保存失败不破坏旧配置。
- 不影响 OBS 主线程、主输出或其他插件。

---
