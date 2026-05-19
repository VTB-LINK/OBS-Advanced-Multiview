# 已知限制 (Known Limitations)

> 本文档记录当前版本 (0.1.x / Phase 1 baseline) 已知的功能缺失和设计限制。
> 这些不是 bug，而是尚未规划或排期到后续 Milestone 的功能。

---

## 布局与显示

- **Layout Preset 管理 UI**：数据结构已预留，但完整的创建 / 切换 / 删除 UI 尚未实现。
- **标签名显示模式**：尚未实现三种显示模式（不显示 / 叠加 / 画面下方）。
- **底色与底图**：不支持为 cell 设定自定义底色或底图。
- **前景 Overlay / 安全区**：不支持叠加 EBU R95 安全区等参考图层。
- **VU Meter**：不支持音频电平表显示。

## 外部信号

- **NDI / Spout 直连**：尚未实现，需后续阶段通过动态调用宿主 obs-ndi / obs-spout2 插件完成。
- **RTMP / HLS / FLV / SRT 拉流**：尚未实现。
- **WebRTC 接入**：尚未实现。
- **Signal Lost / 断流策略**：重试、备播、彩条等断开行为尚未实现。

## 实例管理

- **Move Up / Move Down（排序）**：按钮已禁用，排序持久化未实现。
- **删除已打开实例**：当前删除实例不会自动关闭已打开的对应窗口（窗口可能变为孤儿状态）。后续需实现删除前自动关闭或阻止删除。
- **空 folder 持久化**：空 folder 不会保存到配置文件，重启后消失。只有包含实例的 folder 会恢复。

## Source Identity

- **基于 name 的引用**：当前 Scene / Source assignment 以名称为主键。Source rename / undo / uuid 匹配的更稳健策略排期到后续 Milestone。

## Cell Assignment 保存语义

- **自动保存**：当前 cell assignment 修改（添加/更换/清空）会立即自动保存。Save Cell Assignments 菜单保留为显式保存入口，但语义上与自动保存重叠。后续如需 dirty workflow 需重新设计。

## 分发

- **Installer / Portable 正式 Artifacts**：尚未提供正式安装器或便携版打包脚本，当前仅支持手动复制 DLL。

## 平台

- **macOS / Linux**：CI 构建配置存在，但未经充分运行时测试验证。
