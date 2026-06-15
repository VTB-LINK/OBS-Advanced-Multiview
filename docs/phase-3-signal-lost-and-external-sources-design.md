# Phase 3 Signal Lost 与外部信号设计

> 本文档定义 OBS Advanced Multiview 的 Phase 3（M5~M6）详细开发计划。  
> 术语口径以 [TERMINOLOGY.md](TERMINOLOGY.md) 和 [ROADMAP.md](ROADMAP.md) 为准。  
> 本文档属于专题设计文档，优先级低于 PRD / general instructions / [../README.md](../README.md) / [ROADMAP.md](ROADMAP.md)。如出现冲突，以高优先级文档为准。

---

## 1. Phase 3 范围

Phase 3 包含两个全局 Milestone：

- M5：断开 / 删除 / Signal Lost 行为。
- M6：外部流接入。

Phase 3 的产品目标是把当前仅支持 OBS 内部信号的 Multiview 扩展为可监督外部信号的运行时系统，并且在信号丢失、source 删除、插件缺失、网络断开、协议源消失等情况下保持窗口、OBS 主流程和其它插件稳定。

Phase 3 需要遵守以下最高原则：

- 不使用 while true 空转重试。
- 不在 render 路径里执行阻塞 I/O、网络探测或插件扫描。
- 不直接集成 NDI SDK 或 Spout SDK。
- NDI 通过宿主 DistroAV 插件能力接入。
- Spout 通过宿主 obs-spout2 插件能力接入。
- RTMP / HLS / FLV / SRT 优先通过 OBS 内置 `ffmpeg_source` 接入。
- VLC 作为可选媒体 provider，而不是第一版唯一媒体路径。
- 插件关闭、窗口关闭、cell 清空、布局缩容时，所有 private source、timer、volmeter、texture、weak ref 都必须可释放。
- Signal Lost 与 fallback 行为必须动态生效，无需重启 OBS。

---

## 2. Phase 3 非目标

Phase 3 不做以下内容：

- 正式 installer / portable artifacts。该项属于 Phase 4（M7）。
- 6x6 / 10x10 全量性能矩阵封口。该项属于 Phase 4（M8）。
- 自研 NDI / Spout SDK 接收器。
- 完整素材库、资源复制、项目资源管理系统。
- 完整 WebRTC 实现。WebRTC 在 Phase 3 只做接口预留和后续设计占位，除非后续另行确认技术栈。
- 完整 Layout Preset 管理 UI。
- 完整 OBS Mixer 复刻。
- 多 overlay 图层栈。

---

## 3. 已验证的外部依赖基线

当前可作为 M6 第一版实现基准的 OBS portable 环境：

```text
C:\Downloads\OBS-Studio-32.1.2-Windows-x64
```

该 portable OBS 的插件目录中已经存在：

- `distroav.dll`
- `win-spout.dll`
- `obs-ffmpeg.dll`
- `vlc-video.dll`

当前 VS Code 工作区中可查阅的源码：

- DistroAV：NDI provider 基准。
- obs-spout2-plugin：Spout provider 基准。
- obs-studio 32.1.2：OBS 内置 media source、private source、source type 检测 API 基准。

已确认的 OBS / 插件 source type id：

- DistroAV NDI input：`ndi_source`
- obs-spout2 input：`spout_capture`
- OBS FFmpeg media source：`ffmpeg_source`
- OBS VLC source：`vlc_source`

可用于 source type 检测的 OBS API：

- `obs_enum_input_types()` / `obs_enum_input_types2()`
- `obs_source_get_display_name(id)`
- `obs_get_source_output_flags(id)`
- `obs_get_source_defaults(id)`
- `obs_get_source_properties(id)`

private source 创建 API：

- `obs_source_create_private(id, name, settings)`

---

## 4. 现有架构切入点

当前 Phase 2 代码中，cell 信号绑定由 `CellAssignment` 表示：

```cpp
struct CellAssignment {
    int row;
    int col;
    std::string type; // "pgm", "prvw", "scene", "source", ""
    std::string name;
};
```

当前 `MultiviewWindow` 内部用 `CellSource` 缓存 scene/source weak ref，并在 render 中直接按 `type` 分支处理 PGM / PRVW / Scene / Source。

Phase 3 不应继续把外部协议分支直接塞进 render。应先引入轻量运行时层：

```text
CellAssignment       持久化配置
SignalConfig         持久化的外部信号配置
LostSignalSettings   持久化的断流 / 删除策略
CellSignalRuntime    窗口运行时状态和 private source 引用
SignalProvider       按 source type 创建/更新/释放 runtime
MultiviewWindow      渲染当前 active/fallback/lost 状态
```

Renderer 需要知道的是：

- 当前 cell 是否有 active source 可渲染。
- 当前 cell 是否处于 missing / lost / retry / fallback 状态。
- 当前 cell 是否有 fallback source 或 placeholder image。
- 当前状态 overlay 需要显示什么文字。

Renderer 不应知道：

- NDI receiver 如何循环。
- Spout sender 如何枚举。
- FFmpeg/VLC 如何重连。
- 某个外部插件内部线程如何管理。

---

## 5. 配置模型 v3

Phase 3 应将 `CURRENT_CONFIG_VERSION` 升到 `3`。

v3 新增字段必须向后兼容：

- v1 / v2 配置没有 `signalConfig` 时，仍按现有 `type/name` 加载。
- v1 / v2 配置没有 `lostSignalSettings` 时，使用默认策略。
- 未知 provider 字段必须忽略并记录 warning，不得导致加载失败。
- 配置保存继续使用当前 atomic save 机制。

建议扩展后的概念模型：

```cpp
enum class SignalProviderType {
    ObsProgram,
    ObsPreview,
    ObsScene,
    ObsSource,
    MediaFfmpeg,
    MediaVlc,
    NdiDistroAv,
    Spout,
    WebRtcReserved,
};

struct SignalConfig {
    SignalProviderType provider;
    std::string displayName;
    obs_data_t *providerSettings; // persisted as object in JSON, not held raw in C++
};

enum class InternalMissingBehavior {
    Black,
    PlaceholderImage,
    ClearCell,
};

enum class ExternalLostBehavior {
    SignalLostOverlay,
    RetryOnly,
    RetryWithFallback,
    SignalLostImage,
};

struct LostSignalSettings {
    InternalMissingBehavior internalMissingBehavior;
    ExternalLostBehavior externalLostBehavior;
    std::string placeholderImagePath;
    std::string signalLostImagePath;
    CellAssignment fallbackAssignment;
    int retryInitialMs;
    int retryMaxMs;
    int manualReconnectCooldownMs;
};
```

实际 C++ 实现不建议在 long-lived struct 中保存 raw `obs_data_t *`。可用明确字段或轻量 `ProviderSettings` struct 分组实现。

默认策略：

- 内部 Scene / Source 缺失：黑场 + `Missing Source` overlay + 按名称 lazy re-resolve。
- 外部流不可用：`Signal Lost` overlay + 保持 private source 活着，让 provider 内部机制继续恢复。
- fallback 默认关闭。
- 手动重连冷却默认 1000 ms。
- FFmpeg reconnect delay 默认可映射到 10 秒，遵循 OBS 默认值。

---

## 6. 运行时状态机

建议运行时状态：

```text
Empty
Resolving
Active
MissingInternal
Connecting
Lost
RetryScheduled
FallbackActive
Error
```

状态语义：

- `Empty`：cell 没有 assignment。
- `Resolving`：正在解析 OBS source name 或创建 private source。
- `Active`：有可渲染 source，且健康状态正常。
- `MissingInternal`：内部 Scene / Source 找不到或 strong ref 获取失败。
- `Connecting`：外部 private source 已创建，但还没有有效输出。
- `Lost`：曾经 active，之后失去有效输出。
- `RetryScheduled`：已安排重试或重建。
- `FallbackActive`：主信号不可用，正在显示 fallback。
- `Error`：provider 不可用、source type 缺失、配置无效或创建失败。

状态转移原则：

- 状态变化只能由 UI 线程或受控回调调度到 UI 线程提交。
- render 线程可以读取状态快照，但不能执行重建 source 或文件 I/O。
- private source 重建必须避开 `source_mutex_` 与 graphics lock 的 ABBA 死锁风险。
- window close / clear cell / layout shrink 必须取消 pending retry。

---

## 7. M5 详细设计：Signal Lost 与删除行为

### 7.1 M5.0：运行时层落地

新增 `CellSignalRuntime`，先包装现有内部信号，不改变用户可见行为。

建议字段：

```cpp
struct CellSignalRuntime {
    CellAssignment assignment;
    SignalRuntimeState state;
    OBSWeakSource weakRef;
    OBSSource privateSource;
    bool showing;
    bool prvwFallback;
    uint64_t lastActiveNs;
    uint64_t lastReconnectNs;
    int retryAttempt;
    std::string statusText;
};
```

实现要求：

- `cell_sources_` 可以先重命名或并行保留，逐步迁移。
- PGM / PRVW 仍然每帧 resolve，不缓存 private source。
- Scene / Source 继续使用 name + weak ref + lazy re-resolve。
- 保留现有 `obs_source_inc_showing` / `obs_source_dec_showing` 语义。

### 7.2 M5.1：内部 source missing

触发条件：

- `OBSGetStrongRef(weakRef)` 返回 null。
- 按名称 `obs_get_source_by_name(name)` 找不到。
- source 尺寸为 0 且持续超过短暂 grace period。

默认行为：

- 进入 `MissingInternal`。
- 显示黑场 + `Missing Source` overlay。
- 继续按当前 re-resolve 间隔尝试名称匹配。

可选行为：

- `Black`：保持黑场 + 状态 overlay。
- `PlaceholderImage`：显示用户选择的静态图 + 状态 overlay。
- `ClearCell`：清空 assignment 并释放该 cell。该行为必须由用户显式选择。

### 7.3 M5.2：Signal Lost overlay

overlay 绘制应复用 Phase 2 的坐标语义。

推荐绘制顺序：

```text
gutter / cell background
background image
active source 或 fallback source 或 placeholder
safe area
foreground overlay
Signal Lost / Missing Source status overlay
label
VU meter
PGM / PRVW highlight
```

状态 overlay 应尽量克制：

- 顶部或中央显示简短状态。
- 不遮挡 `Below` label 区域。
- 大小随 cell 尺寸 clamp。
- 小 cell 只显示简短文本或图标占位。

第一版可使用 OBS text source 或直接图形绘制。为了减少 private source 数量，优先考虑直接绘制半透明矩形和简短 cached text source。

### 7.4 M5.3：手动立即重连

右键菜单新增：

```text
Reconnect Now
```

显示规则：

- 仅 cell 有 assignment 时显示。
- 内部 missing、外部 lost、retry scheduled、error 状态启用。
- active 状态禁用或隐藏。

冷却规则：

- 默认 1000 ms。
- 冷却期间重复点击无效，并可记录 debug 日志。
- 不弹窗打断用户。

provider 行为：

- 内部 Scene / Source：立即按 name re-resolve。
- FFmpeg：优先 media restart；必要时重建 private source。
- DistroAV NDI：重建 private source 或 `obs_source_update()` 触发 reset，不调用 DistroAV 内部函数。
- Spout：重建 private source 或 `obs_source_update()` 触发 reset。
- VLC：media restart。

### 7.5 M5.4：fallback

第一版 fallback 支持：

- 静态图片。
- OBS 内部 PGM / PRVW / Scene / Source。

后置 fallback：

- 外部 media URL。
- NDI / Spout fallback。
- fallback chain。

fallback 规则：

- fallback 自身失败时，不再递归 fallback。
- fallback active 时状态应显示 `Fallback` 或 `Fallback: <name>`。
- 主信号恢复后自动切回主信号。
- 切回时释放 fallback source showing ref。

### 7.6 M5.5：动态生效

新增通知入口：

```cpp
void notify_multiview_signal_settings_changed(const std::string &uuid = "");
```

触发场景：

- cell lost settings 修改。
- fallback 修改。
- provider settings 修改。
- source picker 改变外部信号配置。

窗口响应：

- 重新解析 affected cell runtime。
- 不重建整个 display。
- 不影响其它 cell 的 source refs。

---

## 8. M6 详细设计：外部流接入

### 8.1 M6.0：Provider 基础设施

建议先实现 provider registry：

```text
InternalObsProvider
FfmpegMediaProvider
VlcMediaProvider
DistroAvNdiProvider
SpoutProvider
WebRtcReservedProvider
```

每个 provider 至少提供：

```cpp
bool is_available() const;
std::string unavailable_reason() const;
ProviderDefaults defaults() const;
bool create_or_update(CellSignalRuntime &runtime, const SignalConfig &config);
void release(CellSignalRuntime &runtime);
void reconnect(CellSignalRuntime &runtime);
SignalHealth health(const CellSignalRuntime &runtime) const;
```

检测 source type 可使用：

- `obs_source_get_display_name(id) != nullptr`
- `obs_get_source_output_flags(id) != 0`
- `obs_get_source_defaults(id) != nullptr`
- `obs_enum_input_types()` 作为枚举确认。

private source 命名建议：

```text
OBS Advanced Multiview/<instance uuid>/<row>,<col>/<provider>
```

private source 不应添加到 OBS scene list。

### 8.2 M6.1：FFmpeg Media Provider

source id：

```text
ffmpeg_source
```

第一版 settings：

```text
is_local_file = false
input = <url>
input_format = optional
reconnect_delay_sec = 10
buffering_mb = 2
hw_decode = user option
clear_on_media_end = true
restart_on_activate = true
close_when_inactive = false for monitoring use
color_range = auto/default
linear_alpha = false
seekable = false by default for live URLs
```

支持协议：

- RTMP
- HLS / M3U8
- FLV
- SRT
- 其它 FFmpeg 支持的 URL，由用户手动输入。

实现策略：

- Source Picker 新增 `Media` tab。
- 用户输入 URL。
- 默认 provider 选择 `FFmpeg`。
- 创建 private source 后，保持 showing，窗口隐藏时不简单停止 transport。
- 使用 `obs_source_media_get_state()` 作为健康状态参考。

状态映射：

- `OBS_MEDIA_STATE_PLAYING`：Active。
- `OPENING` / `BUFFERING`：Connecting。
- `ERROR` / `ENDED` / 持续无尺寸：Lost 或 Error。
- 手动重连：`obs_source_media_restart()`，失败或无效时重建 private source。

### 8.3 M6.2：DistroAV NDI Provider

source id：

```text
ndi_source
```

settings key：

```text
ndi_source_name
ndi_behavior
ndi_behavior_timeout
ndi_bw_mode
ndi_sync
ndi_framesync
ndi_recv_hw_accel
ndi_fix_alpha_blending
yuv_range
yuv_colorspace
latency
ndi_audio
ndi_ptz
ndi_pan
ndi_tilt
ndi_zoom
```

第一版 UI 字段：

- NDI source name。
- Bandwidth：Highest / Lowest / Audio Only。
- Audio enabled。
- Latency：Normal / Low / Lowest。
- Framesync enabled。
- Hardware acceleration request。
- Keep last frame / clear behavior 可先映射到 DistroAV 自身 behavior + 我们自己的 lost behavior。

第一版默认：

```text
ndi_bw_mode = Highest
ndi_behavior = StopResumeLastFrame
ndi_behavior_timeout = KeepContent
ndi_sync = NDI source timecode
latency = Normal
ndi_audio = true
```

实现策略：

- 不链接 DistroAV 头文件。
- 不调用 DistroAV 内部函数。
- 不使用 NDI SDK。
- 只创建 / 更新 / 销毁 `ndi_source` private source。
- 如果 `ndi_source` source type 不存在，Source Picker 的 NDI tab 显示 unavailable 状态。
- NDI source list 可通过 `obs_get_source_properties("ndi_source")` 读取属性列表，但第一版必须允许手动输入 `ndi_source_name`，避免异步枚举依赖。

健康判断：

- source width/height > 0 且有 recent video：Active。
- private source 存在但 width/height 为 0：Connecting 或 Lost。
- source type 缺失：Error。

手动重连：

- 首选重建 private source。
- 备选 `obs_source_update()` 同 settings 触发内部 reset。

### 8.4 M6.3：Spout Provider

source id：

```text
spout_capture
```

settings key：

```text
spoutsenders
usefirstavailablesender
tickspeedlimit
compositemode
```

第一版 UI 字段：

- Use first available sender。
- Sender name。
- Tick speed：1 / 100 / 500 / 1000 ms。
- Composite mode：Opaque / Alpha / Default / Premultiplied。

默认值：

```text
spoutsenders = usefirstavailablesender
tickspeedlimit = 100
compositemode = Default
```

实现策略：

- 不链接 Spout SDK。
- 不调用 obs-spout2 内部函数。
- 只创建 / 更新 / 销毁 `spout_capture` private source。
- 如果 source type 不存在，Spout tab 显示 unavailable。
- Sender list 可通过 source properties 尝试读取；第一版必须支持手动输入。

健康判断：

- source width/height > 0 且 render 可输出：Active。
- sender 不存在或 texture 缺失：Lost。
- source type 缺失：Error。

手动重连：

- 优先 `obs_source_update()`。
- 不生效时重建 private source。

### 8.5 M6.4：VLC Provider

source id：

```text
vlc_source
```

第一版定位：可选 provider，不作为 media URL 默认实现。

原因：

- VLC settings 是 playlist 模型，UI 和持久化复杂度高于 FFmpeg。
- FFmpeg source 已经覆盖 RTMP / HLS / FLV / SRT 的第一版需求。
- VLC 可以作为用户明确选择时的 fallback provider。

settings key：

```text
playlist
loop
shuffle
playback_behavior
network_caching
track
subtitle_enable
subtitle
```

### 8.6 M6.5：WebRTC 占位

WebRTC 在 Phase 3 不作为第一版闭环阻塞项。

需要预留：

- `SignalProviderType::WebRtcReserved`
- UI disabled placeholder。
- 文档记录后续需确认 transport、信令、鉴权、编解码、线程模型。

---

## 9. Source Picker 与设置 UI

当前 Source Picker 有三类：

```text
Special
Scenes
Sources
```

Phase 3 扩展为：

```text
Special
Scenes
Sources
Media
NDI
Spout
WebRTC (disabled / reserved)
```

UI 原则：

- 内部信号沿用现有列表 + 搜索。
- Media tab 使用 URL 输入 + provider 选择。
- NDI tab 优先显示 source name 输入，可选异步列表。
- Spout tab 优先显示 sender 输入，可选 sender 列表。
- 不可用 provider 显示原因，不崩溃、不隐藏整个 Source Picker。
- 每个外部 tab 有 `Test / Refresh` 按钮可以后置，不作为 M6.1 必须项。

Cell 右键菜单新增：

```text
Reconnect Now
Signal Lost Settings...
```

显示位置建议：

```text
Fullscreen
Always on Top
---
Change Source...
Clear Cell
Cell Display Settings...
Signal Lost Settings...
Reconnect Now
---
Edit Grid...
Global Settings
Close
```

`Signal Lost Settings...` 可先做独立 dialog，避免塞进 Phase 2 的 `CellDisplaySettingsDialog` 导致视觉设置和信号运行策略混在一起。

---

## 10. 健康监督与重连策略

Phase 3 的重连策略分两层：

- provider 内部机制：DistroAV、Spout、FFmpeg、VLC 自己处理协议细节。
- Multiview 监督层：判断是否显示 lost overlay、是否启用 fallback、是否重建 private source。

监督层不应做：

- 自己接收 NDI frame。
- 自己枚举 Spout DX texture。
- 自己开网络拉流线程。
- 自己 busy polling 协议状态。

监督层可以做：

- 每秒或按低频 timer 检查 private source width/height、media state、last active time。
- 用户点击 `Reconnect Now` 时重启或重建 provider。
- lost 超时后切换 fallback。
- 主信号恢复后切回。

建议检测频率：

- active cell：1 Hz 健康检查。
- lost cell：backoff timer 控制重建尝试。
- render 线程只读状态，不主动重连。

Backoff 默认：

```text
initial = 1000 ms
max = 30000 ms
jitter = 10%
manual reconnect cooldown = 1000 ms
```

但对已有内部重连机制的 provider，backoff 只控制 Multiview 监督层重建 private source，不控制 provider 内部 loop。

---

## 11. 不可见 / 最小化策略

PRD 已明确：窗口不可见不应简单等同于停止外部流。

Phase 3 第一版策略：

- 内部 OBS source：沿用现有显示引用机制，可继续优化降帧但不在 Phase 3 强制完成。
- FFmpeg / VLC：默认保持 private source 存活；`close_when_inactive` 默认为 false。
- DistroAV NDI：默认保持 private source 存活，由 DistroAV 自身 showing/active 语义控制内部接收。
- Spout：默认保持 private source 存活。

后续 M8 再量化窗口隐藏 / 多窗口 / 10x10 的资源策略。

---

## 12. 日志与错误处理

日志要求：

- provider unavailable：`LOG_WARNING` 一次，避免每帧刷屏。
- private source 创建失败：`LOG_ERROR`，包含 source id、cell row/col、instance uuid。
- lost/restore 状态变化：`LOG_INFO`，只在状态变化时记录。
- health check 细节：`LOG_DEBUG`。

错误边界：

- source type 缺失时，不创建 private source。
- settings key 缺失时，使用 provider 默认值。
- private source 创建失败时，cell 进入 `Error`，显示状态 overlay。
- fallback 失败时，退回黑场 + overlay。
- 配置解析失败时，不影响其它 cell。

---

## 13. 文件与模块建议

建议新增文件：

```text
src/signal-config.hpp
src/signal-config.cpp
src/signal-runtime.hpp
src/signal-runtime.cpp
src/signal-provider.hpp
src/signal-provider.cpp
src/signal-provider-obs.cpp
src/signal-provider-media.cpp
src/signal-provider-ndi.cpp
src/signal-provider-spout.cpp
src/signal-lost-settings-dialog.hpp
src/signal-lost-settings-dialog.cpp
```

可以后置拆分的文件：

```text
src/external-source-picker-page-media.cpp
src/external-source-picker-page-ndi.cpp
src/external-source-picker-page-spout.cpp
```

不建议：

- 把所有 provider 逻辑继续塞进 `multiview-window.cpp`。
- 让 Source Picker 直接创建 OBS source。
- 在 Provider 中直接操作 UI。

---

## 14. 开发顺序

推荐顺序：

1. 新增文档与验收清单。
2. 配置 v3：`SignalConfig` + `LostSignalSettings` 默认值和持久化。
3. `CellSignalRuntime` 包装现有内部信号，行为保持不变。
4. 内部 missing 状态和 `Missing Source` overlay。
5. `Signal Lost Settings...` dialog。
6. `Reconnect Now` 菜单和内部 re-resolve。
7. fallback 静态图。
8. fallback OBS 内部 scene/source。
9. provider registry 和 source type 检测。
10. FFmpeg media provider。
11. NDI provider。
12. Spout provider。
13. VLC provider 可选落地。
14. Phase 3 验收与文档同步。

---

## 15. 验收矩阵

M5 必须验证：

- 旧 v1 / v2 配置正常加载。
- Scene 删除后不崩溃。
- Source 删除后不崩溃。
- 删除后 undo 或重建同名 source 可恢复。
- missing overlay 显示正确。
- placeholder image 路径不存在时不崩溃。
- clear cell 策略只在用户显式选择时执行。
- fallback 静态图可显示。
- fallback OBS source 可显示。
- 主信号恢复后自动切回。
- `Reconnect Now` 有冷却。
- OBS 退出时 runtime 全释放。

M6 必须验证：

- `ffmpeg_source` source type 检测正常。
- RTMP / HLS / FLV / SRT URL 至少覆盖一组实际可播放测试源。
- FFmpeg 网络源断开后显示 Signal Lost，不崩溃。
- FFmpeg 手动重连可执行。
- `ndi_source` source type 检测正常。
- DistroAV 不存在时 NDI tab 显示 unavailable。
- DistroAV 存在时可创建 private NDI source。
- NDI source name 不存在时进入 lost/error 状态，不刷屏。
- NDI 恢复后切回 active。
- `spout_capture` source type 检测正常。
- Spout sender 不存在时进入 lost/error 状态。
- Spout sender 出现后恢复 active。
- private source 不出现在 OBS 场景列表。
- 清空 cell 后 private source 被释放。
- 关闭窗口后 private source 被释放。
- 布局从 10x10 缩小到 1x1 时越界 runtime 被释放。
- VU meter 对有音频的 external private source 可工作；无音频时安全隐藏或空表。

跨版本 / 环境：

- OBS 31.1.1：内部 source + M5 行为必须可用。
- OBS 32.1.2 portable：M5 + FFmpeg + DistroAV + Spout 必须验证。
- OBS 32.0：至少验证不因 API 差异崩溃。

---

## 16. 风险与缓解

### 16.1 外部插件 schema 变更

风险：DistroAV 或 obs-spout2 后续版本修改 settings key。

缓解：

- 记录当前源码基线。
- provider 初始化时使用 `obs_get_source_properties(id)` 做可用性探测。
- 缺失关键属性时显示 unavailable 或 degraded。
- 保留手动输入路径。

### 16.2 private source 生命周期复杂

风险：重建 source 时与 render 线程、volmeter、showing ref 交错。

缓解：

- 所有 runtime release 走统一函数。
- graphics lock 和 `source_mutex_` 顺序沿用 Phase 2 hardening 经验。
- private source 创建/销毁不在 render 回调内执行。

### 16.3 重连和 provider 内部机制冲突

风险：监督层频繁重建 source，与 DistroAV/FFmpeg 内部重连竞争。

缓解：

- 默认保持 private source 活着。
- backoff 只控制监督层主动重建。
- `Reconnect Now` 有冷却。
- 状态变化才写日志。

### 16.4 资源占用上升

风险：10x10 外部源 private source 数量过多。

缓解：

- Phase 3 先保证正确性。
- M8 再做量化降帧、隐藏窗口策略、provider 资源池等优化。
- 文档和 UI 提醒用户大网格外部流会增加资源占用。

---

## 17. Open Questions

需要在实现前或实现中确认：

- Signal Lost overlay 的默认视觉样式是否使用内置图形，还是先用文字 + 半透明背景。
- fallback 静态图是否复用 Phase 2 background image loader，还是单独 runtime image loader。
- NDI source list 是否第一版做异步枚举，还是只做手动输入。
- Spout sender list 是否第一版做刷新按钮，还是只做手动输入。
- FFmpeg 和 VLC 是否在同一个 Media tab 中提供 provider 下拉，还是 VLC 后置到高级选项。
- M6 是否需要把 provider availability 显示到 Settings tab 的诊断区。

---

## 18. Phase 3 完成定义

Phase 3 完成时，OBS Advanced Multiview 应具备：

- 对 OBS 内部 source 删除/恢复的可见、安全、可配置行为。
- 对外部源断开/恢复的可见、安全、可配置行为。
- Signal Lost overlay。
- 手动立即重连。
- fallback 静态图和 OBS 内部 source。
- FFmpeg 网络媒体 private source。
- DistroAV NDI private source。
- obs-spout2 Spout private source。
- 外部 provider 不可用时的优雅降级。
- 旧配置兼容。
- 动态生效。
- OBS 退出和窗口关闭时无残留 runtime。

只有这些闭环完成并通过验收后，才建议进入 Phase 4（M7~M8）的正式 artifacts、性能、稳定性和跨平台回归。
