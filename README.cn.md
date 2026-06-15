# OBS 高级多视图

OBS 高级多视图是一个高度可配置的 OBS Studio 多画面监看插件。它面向需要超越 OBS 内置多视图能力的使用场景：自定义网格、场景与源单元格、纯音频监看、视觉叠加、安全区域、电平表、信号丢失处理，以及外部信号提供器。

[English](README.md)

## 功能特性

- 多个独立的多视图窗口，每个窗口拥有独立布局和设置。
- 自定义网格布局：行列数量、合并单元格、可配置间距。
- OBS 内部单元格：输出、预览、场景、源、纯音频源。
- 外部信号单元格：
  - FFmpeg 媒体 URL 和本地文件
  - DistroAV NDI 源
  - obs-spout2 Spout 发送器
  - VLC 播放列表
  - WebRTC 预留占位
- 全局、实例、单元格三层视觉设置：
  - 背景填充与背景图片
  - 标签
  - 安全区域参考线
  - 电平表
  - 叠加层
  - PGM/PRVW 高亮边框
- 内部源与外部源的信号丢失行为：
  - 源缺失叠加层
  - 占位图片与信号丢失图片
  - 重新连接与重播操作
  - 后备模式
- 场景点击切换：点击场景单元格，在工作室模式下送入预览，非工作室模式下直接切到输出。
- 英文与简体中文界面本地化。

## 环境要求

- OBS Studio 31.1.1 或更新版本。
- Windows 是当前主要测试平台。
- 外部信号提供器需要可选宿主插件：
  - NDI 单元格需要 DistroAV
  - Spout 单元格需要 obs-spout2
  - VLC 播放列表单元格需要 OBS VLC 源支持

项目构建系统支持 macOS 和 Linux，但当前验证重点仍是 Windows。

## 安装

从 GitHub Releases 下载发布压缩包。

对于 OBS portable 版本，请使用 portable 压缩包，并解压到 OBS 根目录。最终目录结构应包含：

```text
obs-plugins/64bit/obs-advanced-multiview.dll
data/obs-plugins/obs-advanced-multiview/locale/en-US.ini
data/obs-plugins/obs-advanced-multiview/locale/zh-CN.ini
```

重启 OBS，然后从以下菜单打开插件：

```text
工具 -> OBS Advanced Multiview
```

开发或本地测试时，可使用部署脚本将最新构建复制到已配置的 OBS portable 目录：

```powershell
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
```

## 快速开始

1. 打开 `工具 -> OBS Advanced Multiview`。
2. 创建一个实例。
3. 设置行数、列数、合并单元格和间距。
4. 在多视图窗口中右键单元格，选择 `添加源...`。
5. 使用 `单元格显示设置...`、`信号丢失设置...` 和 `实例视觉设置...` 调整显示效果。

## 从源码构建

项目使用 OBS 插件模板构建系统、CMake、Qt 6、C++17 和 OBS Frontend API。

Windows 构建：

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo --target obs-advanced-multiview
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
```

首次配置和故障排除请阅读 [docs/setup/README.md](docs/setup/README.md)。

## 文档

- [开发工作流](docs/DEVELOPMENT.md)
- [环境配置指南](docs/setup/README.md)
- [分发说明](docs/setup/DISTRIBUTION.md)
- [路线图](docs/ROADMAP.md)
- [已知限制](docs/known-limitations.md)
- [术语规范](docs/TERMINOLOGY.md)

设计与实现说明位于 [docs](docs/) 目录。历史项目计划已移动到 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 当前状态

1.0 发布候选版聚焦于 Windows 使用体验、自定义多视图布局、OBS 内部源监看、FFmpeg/NDI/Spout/VLC 外部信号提供器、信号丢失处理、视觉自定义，以及英文 / 简体中文双语界面。

## 许可证

本项目使用 GPL-2.0-or-later 许可证。详见 [LICENSE](LICENSE)。