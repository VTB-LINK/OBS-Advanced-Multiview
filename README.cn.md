# OBS 高级多视图

OBS 高级多视图是一个 OBS Studio 插件，用于扩展 OBS 原生 Multiview。

OBS 原生 Multiview 可以完成基础监看，但布局固定，信号类型有限，单元格显示和信号丢失处理也比较有限。本插件增加了自定义布局、合并单元格、单元格级显示设置、外部信号单元格、纯音频单元格、信号丢失处理，以及多个可保存的多视图实例。

它适用于需要在 OBS 内完成更完整监看的场景，例如导播、推流操作、音频监看、VTuber 制作、活动制作，或任何觉得原生 Multiview 不够灵活的工作流。

[English](README.md)

| 管理窗口 | 多视图窗口 |
| --- | --- |
| ![OBS 高级多视图管理窗口](docs/images/amv-manager.png) | ![OBS 高级多视图窗口](docs/images/amv-instance-window.jpg) |

## 和 OBS 原生 Multiview 的区别

OBS 高级多视图保留原生 Multiview 的基本用途，但去掉了一些固定限制。

- OBS 原生 Multiview 使用固定布局。本插件支持**自定义行数、列数、合并单元格和间距**。
- OBS 原生 Multiview 主要监看 Program、Preview 和场景。本插件还可以监看**单个源、纯音频源、媒体 URL / 文件、NDI、Spout 和 VLC 播放列表**。
- OBS 原生 Multiview 的显示样式基本固定。本插件支持**全局、实例、单元格三层显示设置**。
- OBS 原生 Multiview 没有细分到单元格的信号丢失处理。本插件可以显示**源缺失、Signal Lost、占位图、fallback 状态和重连操作**。
- OBS 原生 Multiview 是一组固定监看。本插件可以保存多个 Multiview 实例，每个实例有独立布局和设置。
- OBS 原生 Multiview 无法创建外部监看信号。本插件会尽可能用 OBS 私有源创建外部 provider 单元格，不需要把每一路监看信号加入正式场景。

## 功能

### 布局和窗口

- 多个可保存的 Multiview 实例。
- 当前版本每个实例同时打开一个窗口。
- **自定义行数和列数。**
- **合并单元格 / span 区域。**
- 0 到 50 px 的间距设置。
- 支持零间距布局，并保留 PGM/PRVW 高亮边框。
- 布局和信号分配随 OBS 场景集合保存。

### OBS 内部信号监看

- Program 单元格。
- Preview 单元格。
- 场景单元格。
- 源单元格。
- **纯音频源单元格。**
- 场景点击切换：点击场景单元格，在工作室模式下送入预览，非工作室模式下直接切到输出。
- 可选的双击场景单元格切换到输出。

### 外部信号单元格

- **FFmpeg 媒体 URL 和本地文件。**
- **DistroAV NDI 源。**
- **obs-spout2 Spout 发送器。**
- OBS 支持 VLC 源时可使用 **VLC 播放列表单元格**。
- WebRTC 目前是预留 provider，运行时尚未实现。
- NDI 和 Spout 通过宿主 OBS 插件访问。本插件不内置 NDI SDK，也不内置单独的 Spout SDK。

### 视觉设置

- **全局视觉设置。**
- **实例视觉设置。**
- **单元格视觉设置。**
- 背景颜色。
- 背景图片。
- 标签显示模式。
- 安全区域参考线。
- 前景叠加图片。
- PGM/PRVW 高亮边框。
- PGM/PRVW 高亮支持**嵌套场景检测**。
- **VU Meter。**
- VU 峰值保持。
- VU dB 刻度和标签。
- VU 根据源声道数显示**多声道电平条**。
- **VU RMS / magnitude 指示条。**

### 信号丢失处理

- **源缺失叠加提示。**
- **Signal Lost 叠加提示。**
- 占位图片。
- Signal Lost 图片。
- Fallback 图片。
- 支持时可 fallback 到 PGM、PRVW、场景或源。
- **Reconnect Now** 操作。
- 外部 provider 的 retry 和 fallback 行为。
- 支持的媒体 provider 提供 Replay、Previous、Play/Pause、Next 操作。

### 工作流细节

- **单元格右键菜单**支持信号分配、信号编辑、显示设置、信号丢失设置、重连和媒体控制。
- 设置保存在 OBS 插件配置目录中。
- 英文与简体中文界面本地化。
- 基于 OBS 插件模板、Qt 6、C++17、libobs 和 OBS Frontend API。
- 当前 release candidate 主要验证 Windows 平台。

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

## 合并单元格

单元格合并在管理窗口里完成，不在 Multiview 渲染窗口里完成。

1. 打开 `工具 -> OBS Advanced Multiview`。
2. 在左侧列表选择一个实例。
3. 在实例详情面板设置网格 `Rows` 和 `Cols`。
4. 在网格预览区域点击单元格进行选择。普通点击选择一个单元格。`Shift` + 鼠标左键点击可以从上一次点击的单元格到当前单元格快速选择一个矩形区域，这是合并前最快的选择方式。`Ctrl` + 点击可以切换当前选择中的单元格。点击已有 span 会选中整个 span 区域。
5. 点击 `Merge`，把当前选择合并成一个 span。
6. 选中已合并单元格后点击 `Unmerge` 可以取消该 span，或点击 `Reset All` 清除当前布局里的全部 span。

合并规则：

- 选中的单元格必须组成一个填满的矩形。
- 1x1 选择不是合并。
- 合并矩形必须在当前网格范围内。
- 合并矩形不能只覆盖已有 span 的一部分。
- 如果合并矩形完整包含一个或多个已有 span，这些 span 会被吸收到新的合并区域中。
- 如果缩小行数或列数，超出新网格范围的 span 会被安全移除。

## 从源码构建

项目使用 OBS 插件模板构建系统、CMake、Qt 6、C++17 和 OBS Frontend API。

Windows 构建：

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo --target obs-advanced-multiview
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
```

首次配置和故障排除请阅读 [docs/setup/README.md](docs/setup/README.md)。

### NDI 输出（可选）

内置的 **NDI 外部输出**仅在构建时安装了 [NDI SDK](https://ndi.video/for-developers/ndi-sdk/) 才会被编译。构建会自动探测默认安装路径（如 `C:\Program Files\NDI\NDI 6 SDK`）；可用环境变量 `NDI_SDK_DIR` 覆盖。若未找到 SDK，CMake 会打印提示并关闭 `ENABLE_NDI_OUTPUT`——插件仍可构建，但 NDI 后端被排除、**无法修改或测试**。Spout 输出无此要求（其 SDK 已随仓库 vendored）。

运行时插件**动态加载** NDI 运行时 DLL（不打包 DLL），因此终端用户需安装 [NDI 运行时](http://ndi.link/NDIRedistV6)（或 NDI Tools）。NDI 5 与 NDI 6 运行时均可。

## 文档

- [开发工作流](docs/DEVELOPMENT.md)
- [环境配置指南](docs/setup/README.md)
- [分发说明](docs/setup/DISTRIBUTION.md)
- [路线图](docs/ROADMAP.md)
- [已知限制](docs/known-limitations.md)
- [术语规范](docs/TERMINOLOGY.md)

设计与实现说明位于 [docs](docs/) 目录。项目里程碑和后续规划见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 当前状态

1.0 发布候选版聚焦于 Windows 使用体验、自定义多视图布局、OBS 内部源监看、FFmpeg/NDI/Spout/VLC 外部信号提供器、信号丢失处理、视觉自定义，以及英文 / 简体中文双语界面。

## 许可证

本项目使用 GPL-2.0-or-later 许可证。详见 [LICENSE](LICENSE)。
