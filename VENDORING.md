# Vendored 依赖清单与升级评估

本插件跑在用户的直播 OBS 进程里，稳定优先（见 `AGENTS.md §0`）。vendoring 的核心取向是
**能复用 OBS 就绝不自带 SDK**：DeckLink / AJA 输出复用 OBS 已注册的 `decklink_output` /
`aja_output`，FFmpeg / VLC 源复用 OBS 的 `ffmpeg_source` / `vlc_source`——**零 SDK 依赖、零
链接、零分发**。真正 vendored 的只有两个不得不带的组件（Spout、NDI 头），如下。

本文件是**当前状态总览 + 升级评估流程**；具体拉取/复现命令在各 per-vendor 文档（下表末列），
本文件不重复。

## 当前钉死版本

| 依赖 | 用途 | 上游来源 | 版本（钉死） | 许可 | 集成方式 | 入 git | 详见 |
|---|---|---|---|---|---|---|---|
| **Spout2（SpoutDX）** | Spout 输出：D3D11 共享纹理发送多画面 | `leadedge/Spout2`（git submodule） | **2.007.017**（commit `f49e2f46`，2025-10-22；= 当前上游最新 release） | BSD-2-Clause | vendored 子模块 `deps/Spout2`；只编 `SpoutDX_static` 的源子集（`SpoutDX.cpp` + `SpoutGL` 子集）为 `spout-dx` 静态库，静态链入插件；`/MD` 运行时、告警关闭、不拉 Spout 根工程 | 是（submodule 引用） | [`cmake/spout-output.cmake`](cmake/spout-output.cmake) |
| **NDI SDK（headers）** | NDI 输出：帧发送多画面 | NDI 6 SDK（NewTek / Vizrt） | headers 复制自 **NDI 6 SDK** `Include/`（verbatim）；运行时用 **ABI 冻结的 v5 函数子集**（`NDIlib_v5_load`，兼容 NDI 5 与 6 运行时） | NDI SDK EULA（`deps/ndi/NDI SDK License Agreement.pdf`） | **headers-only** vendored 于 `deps/ndi/include`（CI / 无 SDK 者可编译）；`cmake/ndi-output.cmake` 优先系统 SDK、回退 vendored 头。**不 vendored、不分发** import lib 或运行时 DLL——运行时经 Qt `QLibrary` 动态载 `Processing.NDI.Lib.x64.dll`（见 `src/multiview-ndi-runtime.cpp`） | 是（头） | [`deps/ndi/README.md`](deps/ndi/README.md) |

> 全部**有意钉死**。广播级工具稳定优先：升级永远是**主动、评估过、测过**的动作，绝不自动跟版——
> currency 不是目标，确定性是。

### 非 vendored（复用 / 外部构建依赖，列此备查）

- **DeckLink 输出 / AJA 输出**：复用 OBS 已注册的 `decklink_output` / `aja_output`（`obs_output_create`
  + 自建 `video_t`），**零 SDK**；可用性运行时探测 `obs_get_output_flags(...)`。设计见
  `docs/issue-16-decklink-output-design.md`、`docs/issue-18-aja-output-design.md`。
- **FFmpeg / VLC 源**：复用 OBS 的 `ffmpeg_source` / `vlc_source`（`obs_source_create_private`），零 SDK。
- **libobs / Qt6 / obs-frontend-api**：外部构建依赖，`find_package` 解析；OBS 版本随 `buildspec.json`
  钉死（当前 31.1.1）。obs-plugintemplate 脚手架在 `build-aux/` + `cmake/`。均非本仓 vendored 源。

## 当前评估（2026-09）

- **Spout2 2.007.017**：**已是上游最新**（release 2.007.017，2025-10-22）。无需更新。
- **NDI headers（NDI 6）**：上游 SDK 最新 6.3。本插件**只用冻结的 v5 函数子集**，头文件的 6.x 点版本
  对我们无功能影响，**无需更新**——除非将来要用 NDI 6 独有 API（当前不需要）。
- 无必须更新项；无已知安全 / ABI 阻断。

## 升级某依赖前，评估什么

1. **读上游 changelog / release notes**：破坏性变更、安全修复、我们真需要的新特性。无强理由不升。
2. **ABI / 契约兼容**（各依赖专属风险点）：
   - **Spout2**：只编 `SpoutDX_static` 子集——上游若增删该子集依赖的源文件，按编译报错增补 `cmake/spout-output.cmake` 的源列表；确认仍走 `SPOUT_BUILD_STATIC` + `/MD`（不继承 Spout 的 `/MT` 默认）。
   - **NDI**：只依赖 v5 冻结子集，头升级低风险；若换头，用上游同名文件覆盖 `deps/ndi/include/`（见 `deps/ndi/README.md`），并确认仍只调 `NDIlib_v5_load` 那批签名。
3. **许可 re-check**：确认许可未变（Spout BSD-2 / NDI EULA）；EULA 的分发条款尤其复核（我们不分发 NDI 运行时，只动态载系统装的 DLL）。
4. **安全**：查上游 CVE / 安全公告；安全版是强升级理由。

## 升级步骤（通用）

拉取/复现命令在各 per-vendor 文档，此处只给流程：

1. 走代理拉新版（github：`$env:HTTP_PROXY="http://127.0.0.1:31333"; $env:HTTPS_PROXY="http://127.0.0.1:31333"`；本地/内网直连不走代理）。Spout 更新子模块：`git -C deps/Spout2 fetch && git -C deps/Spout2 checkout <tag>`，再于主仓 `git add deps/Spout2`。NDI 按 `deps/ndi/README.md` 覆盖头。
2. 按各 per-vendor 文档 / 本文件的「专属风险点」重新套用改动。
3. 按 `AGENTS.md §3` 跑：clang-format → 构建 `build_x64`（Debug + RelWithDebInfo）→ `deploy-plugin.ps1`。
4. 真机冒烟：对应后端推一遍（Spout→Spout 接收端出画；NDI→NDI 接收端出画）。
5. 提交（`AGENTS.md §4`）：信息写清 `<依赖> <旧版>→<新版>` + 动机（安全 / 特性 / 兼容），无 AI 署名。
