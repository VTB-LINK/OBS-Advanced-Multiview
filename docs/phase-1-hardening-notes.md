# Phase 1 Hardening Notes

> 本文档记录 Milestone 4 之前需要关注的代码硬化观察项。
> 分为"已修复"和"观察 / 后续处理"两类。

---

## ConfigManager

### 已修复

- **空路径保护**：如果 `config_dir_` 为空，`load()` / `save()` 记录错误并返回 false，避免写入未知工作目录。
- **未使用变量**：删除 `save_to_file()` 中未使用的 `tmp_path` 局部变量。

### 观察项

- **parse 失败后的 fallback 策略**：当前 `load_from_file()` parse 失败时返回 false，不会重置到安全默认值。后续如需更稳健策略：parse 失败时保留内存已有配置，或重置为 safe defaults，而非让 UI 假设 config 正常加载。
- **configVersion migration**：version 字段已预留但无实际迁移逻辑，后续配置格式变更时需补充。

---

## LayoutEngine

### 观察项

- **compute() clamp vs validate() reject 语义差异**：
  - `compute()` 对越界 span 做静默 clamp（防御性 fallback，防崩）。
  - `validate_span()` / `validate_all_spans()` 对越界返回 `OutOfBounds`（UI 保存前拒绝非法配置）。
  - 当前两者配合工作，文档需明确 compute 的 clamp 是防御性 fallback，不代表配置合法。

- **极小 viewport + 大 gutter**：当前 `avail_w < cols` 时强制设为 `cols`，避免负值，但可能导致 cell 超出 viewport。需验收覆盖：
  - viewport 小于 gutter 总和
  - 10x10 + gutter 50
  - window 极小尺寸
  - hit-test 是否仍安全

---

## MultiviewWindow

### 观察项

- **OBSDisplay 释放确认**：`destroy_display()` 置空 `OBSDisplay` 依赖 RAII wrapper 释放底层资源。需确认 `obs.hpp` 中 `OBSDisplay` 的析构行为符合预期。

- **render 中 source_mutex_ 持有时间**：当前 `render()` 在整个绘制周期持有 `source_mutex_`，UI 线程调用 `refresh_sources()` 或 `release_source_refs()` 时可能等待渲染完成。后续 Milestone 4 加 overlay / VU meter 后可能加重锁持有时间。
  - 后续考虑将 cell source 快照复制到局部变量，缩短 `source_mutex_` 持有时间。
  - 当前不建议贸然大改，避免破坏稳定性。

- **obs_get_source_by_name 依赖 name**：当前 Scene / Source 引用按 name 获取。后续 Milestone 需补充更稳健的 source identity / rename / delete / undo 策略。

- **置顶切换 window 重建**：always-on-top 先 destroy display 再 setWindowFlags 再 recreate，时序较为敏感。需确认在连续快速切换时不产生 race condition。

---

## ManagerDialog

### 已修复

- **Move Up / Move Down 禁用**：排序功能未实现前，按钮已禁用并更新 tooltip 提示。

### 观察项

- **删除已打开实例**：`on_delete_instance()` 删除 config 中实例后，对应已打开 MultiviewWindow 可能变成孤儿。后续需给 `plugin-main.cpp` 增加"关闭指定 uuid 窗口"全局函数，ManagerDialog 删除前调用。

- **空 folder 不持久化**：空 folder 当前不会持久化；只有有实例引用的 folder 会恢复。这是已知设计，非 bug。

---

## Source Lifecycle

### 观察项

- **on_add_source() 立即 save**：当前 cell assignment 修改会立即保存配置。Save Cell Assignments 菜单保留为显式入口但语义重叠。后续如需 dirty workflow 需重新设计。

- **source showing / dec_showing 配对**：需验证在多窗口、多 cell 引用同一 source 场景下关闭窗口时 dec_showing 正确配对。

---

## CI / Build

### 观察项

- **macOS 构建**：CI 配置存在但未验证运行时行为。
- **Linux 构建**：需确认 CI 最新主线构建成功。

---

## Documentation Hygiene

### 已修复

- **plan.md 乱码**：修正 `等��` 为 `等。`
- **旧模板名残留**：docs 中 `plugintemplate-for-obs` 已统一修正为 `obs-advanced-multiview`。
- **SETUP.md 过时描述**：`src/plugin-main.c` 引用已修正为 `src/plugin-main.cpp`。
