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

### 已修复

- **置顶切换 window 重建**：Windows 平台已改用 Win32 `SetWindowPos(HWND_TOPMOST/HWND_NOTOPMOST)` 实现，不再调用 `setWindowFlags`，不会触发 HWND 重建，无 flicker / 焦点丢失。
- **obs_get_source_by_name 依赖 name**：实现 lazy re-resolve 机制——当 source 被删除（`weak_ref` 失效）时按保存的 name 重新查找。支持 undo 恢复场景。re-resolve 频率可在 Global Settings 配置（继承 OBS fps 或自定义 1~120 fps）。
- **PRVW fallback 颜色**：无 Studio Mode 时的指示条颜色已修正为黄色（`0xCCFFD400`），与 PGM 红色条区分。

### 观察项

- **OBSDisplay 释放确认**：`destroy_display()` 置空 `OBSDisplay` 依赖 RAII wrapper 释放底层资源。需确认 `obs.hpp` 中 `OBSDisplay` 的析构行为符合预期。

- **render 中 source_mutex_ 持有时间**：当前 `render()` 在整个绘制周期持有 `source_mutex_`，UI 线程调用 `refresh_sources()` 或 `release_source_refs()` 时可能等待渲染完成。后续 Milestone 4 加 overlay / VU meter 后可能加重锁持有时间。
  - 后续考虑将 cell source 快照复制到局部变量，缩短 `source_mutex_` 持有时间。
  - 当前不建议贸然大改，避免破坏稳定性。

---

## ManagerDialog

### 已修复

- **Move Up / Move Down 禁用**：排序功能未实现前，按钮已禁用并更新 tooltip 提示。
- **Folder 功能已移除**：Milestone 3 后决定移除 folder grouping 功能，简化实例管理。空 folder 不持久化问题已不存在。

### 观察项

- **删除已打开实例**：`on_delete_instance()` 删除 config 中实例后，对应已打开 MultiviewWindow 可能变成孤儿。后续需给 `plugin-main.cpp` 增加"关闭指定 uuid 窗口"全局函数，ManagerDialog 删除前调用。

---

## Source Lifecycle

### 已修复

- **Source 删除 + Undo 恢复**：通过 lazy re-resolve 机制支持。删除后 weak_ref 失效不崩溃，undo 恢复后自动按名重建引用。
- **Save Cell Assignments 菜单语义重叠**：已移除独立的 Save Cell Assignments 菜单项，改为纯自动保存。

### 观察项

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
