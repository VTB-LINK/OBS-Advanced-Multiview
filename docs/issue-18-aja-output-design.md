# Issue #18 — AJA 硬件输出设计文档

状态：设计定稿，待实现（测试后置：无 AJA 硬件，先实现+提交，靠可用性灰置兜底）
范围：**仅 AJA**（复用 OBS 已注册的 `aja_output`，零 NTV2 SDK 依赖）
关联：issue #16 DeckLink 输出（`docs/issue-16-decklink-output-design.md`，同一「复用 obs_output + 自建 video_t」范式）、issue #11 外部输出层（Spout/NDI）、A1/A2/A3 输出注册表重构

> 用户已定：**完整控制** —— 对话框暴露 OBS 原生 AJA 对话框的全部旋钮
> （device / 输出连接口 IOSelection / videoFormat / pixelFormat / SDITransport / SDITransport4K）。

---

## 0. 最高原则（先过这四条）

1. **绝不炸 OBS**：`aja_output` 的 `create` 就地做重活（枚举卡、`AcquireOutputSelection` 占用通道、`Initialize` DMA 黑帧 + 等垂直中断、`CreateThread(true)` 立即起输出线程）；`destroy` 会 `StopThread()` join 该线程。`video_t` 被该输出的数据捕获线程与本插件图形线程双向访问，销毁顺序错 = use-after-free。
2. **非阻塞**：create（等垂直中断 + 起线程）与 destroy（join 线程 + 释放卡）均为同步、可达数~十 ms。**绝不能在 OBS graphics 线程上同步执行**，否则 hitch 主节目输出。
3. **非入侵 / 隔离**：AJA 输出失败（卡被占、IOSelection 冲突、格式无效）只降级为「不出画 + 日志 + 冷却重试」，不连累其它后端 / 实例 / OBS 本体。
4. **广播级稳定**：环满丢帧、启动失败冷却、拆解幂等、反复启停不崩不卡。

### 与 DeckLink 的根本差异（务必先读，否则照搬 DeckLink 会错）

| 维度 | DeckLink（issue #16 已实现） | AJA（本设计） | 依据 |
|---|---|---|---|
| 光栅获取 | `create` 内即 `obs_output_set_video_conversion`，`create` 后 `obs_output_get_video_conversion` 立即返回模式宽高 → 可探针拿光栅、锁定合成尺寸零缩放 | **`create` 不设 conversion**，仅 `start` 里设（`aja-output.cpp:1093`）；`create` 后 `get_video_conversion` 返回 **NULL**。**无法在对话框/建 video_t 前拿到光栅** | aja `create` 934-1013 无 conversion；`start` 1082-1093 才设；`obs-output.c:1381` `video_conversion_set` 为假返回 NULL |
| 喂帧尺寸 | video_t 开在精确 SDI 光栅，合成即光栅，零缩放 | video_t 开在**画布尺寸**，`start` 把 conversion 设成光栅 → **靠 libobs 原始视频缩放器**缩放到光栅（OBS 原生 aja-ui 预览正是此法） | aja-ui `preview_output_start` video_t=canvas BGRA（`aja-ui-main.cpp:127-160`）；libobs 缩放器 `video-io.c:340-366` |
| 无效配置后果 | `mode_id` 无效/0 → `decklink_output_create` **空指针解引用崩 OBS**（H2 硬前置） | 无效值（IOSelection Invalid / format UNKNOWN / pixel INVALID）→ `aja_output_create` **安全返回 nullptr**，不崩 | aja `create` 969-976 逐项校验后 `return nullptr` |
| 可用性 | decklink 插件随 OBS 分发即注册，无卡也 available | **无卡不注册**：`aja/main.cpp:20` `numDevices==0` 时整个 aja 模块 `return false`，`aja_output` 根本不注册 → 天然硬件感知 | `aja/main.cpp:16-32` |
| 像素格式 | 输出转换固定 BGRA，喂 BGRA 零转换 | 仅 **UYVY(8bit YCbCr)** 与 **BGR3(24bit BGR)** 两种，**无 BGRA**，喂 BGRA 必经转换 | pixel list `{8BIT_YCBCR, 24BIT_BGR}`（`aja-output.cpp:848`）；映射 `aja-common.cpp:247-252` |
| FPS | `start` 硬查 `IsEqualFrameRate`，不等则启动失败 | **`start` 不硬查**；videoFormat 自带固定 fps，画布 fps 不符靠输出线程丢/重帧（`mVideoAdjust`）软对齐，**不失败但会 A/V 漂移** | aja `start` 1022-1110 无 fps 断言；对齐逻辑 `OutputThread` 722-742 |
| 每输出唯一 ID | 无（device_hash 天然唯一） | **必须**每输出唯一 `kUIPropAJAOutputID`（owner 串），否则 CardManager 通道占用误判 | 见 §6 |

---

## 1. aja_output 完整契约（逐项，均已读源码核实）

### 1.1 注册与形态
- `register_aja_output_info()`（`aja-output.cpp:1221-1238`）：`id = "aja_output"`、`flags = OBS_OUTPUT_AV`（原始音视频，`:1226`），回调 create/destroy/start/stop/raw_video/raw_audio/update/get_defaults/get_properties。
- **注册前置**：`aja/main.cpp:16-32` `obs_module_load` 在 `CNTV2DeviceScanner::GetNumDevices()==0` 时 `return false`——**没有 AJA 卡则整个 aja 模块不加载**，`aja_output` 不注册。故 `obs_get_output_flags("aja_output")!=0` 同时蕴含「OBS 用 NTV2 SDK 构建过」+「启动时插着 AJA 卡」。热插拔在 OBS 启动后无效（模块已跳过加载）。

### 1.2 设置项清单（每项：id 字符串 / 类型 / 枚举来源 / 联动）

所有 id 字符串定义在 `aja-ui-props.hpp`；下拉项在 `aja_output_get_properties`（`aja-output.cpp:1179-1205`）建空壳、由回调填充。

| kUIProp | id 字符串 | OBS 类型 | 值来源 / 枚举 | 备注 |
|---|---|---|---|---|
| `kUIPropDevice` | `"ui_prop_device"` | LIST/STRING | `populate_output_device_list`（`:772-798`）枚举 CardManager 卡，data=`GetCardID()`（`设备ID串_序列号`） | 变更触发 `aja_output_device_changed` |
| `kUIPropOutput` | `"ui_prop_output"` | LIST/INT | `populate_io_selection_output_list`（`aja-common.cpp:94-118`）= `IOSelection` 枚举（`aja-enums.hpp:23-48`），首项「Select…」=`Invalid(22)`，逐项 `DeviceCanDoIOSelectionOut` 过滤、`filter_io_selection_output_list` 把**被其它 owner 占用**的项灰置 | 变更触发 `aja_output_dest_changed` |
| `kUIPropVideoFormatSelect` | `"ui_prop_vid_fmt"` | LIST/INT | `populate_video_format_list`（`aja-common.cpp:120-175`）= `NTV2VideoFormat` 枚举，`NTV2DeviceCanDoVideoFormat` 过滤 + **按画布 fps 过滤**（`matchFPS=MATCH_OBS_FRAMERATE`，见 §1.6） | 变更触发 `aja_video_format_changed` |
| `kUIPropPixelFormatSelect` | `"ui_prop_pix_fmt"` | LIST/INT | `populate_pixel_format_list(deviceID, {NTV2_FBF_8BIT_YCBCR, NTV2_FBF_24BIT_BGR}, list)`（`aja-output.cpp:848`）——**输出仅两种像素格式** | 无联动回调 |
| `kUIPropSDITransport` | `"ui_prop_sdi_transport"` | LIST/INT | `populate_sdi_transport_list`（`aja-common.cpp:187-206`）= `SDITransport` 枚举（`aja-enums.hpp:50-58`）；**输出侧禁用 12G**（`:200`，AJA 4K HFR bug），6G 仅 `NTV2DeviceCanDo12GSDI` 卡 | 可见性随 IOSelection |
| `kUIPropSDITransport4K` | `"ui_prop_sdi_transport_4k"` | LIST/INT | `populate_sdi_4k_transport_list`（`aja-common.cpp:208-214`）= `{Squares, 2SI}`（`SDITransport4K`，`aja-enums.hpp:60`） | 可见性随 IOSelection 且 4K 格式 |
| `kUIPropAJAOutputID` | `"aja_output_id"` | STRING（**不注册为可见属性**） | 每输出唯一 owner 串；仅由 `create`/`start`/`stop` 从 settings 读，供 CardManager 通道占用跟踪（`aja-ui-props.hpp:26-34`） | **本插件必须自生成、保证唯一**，见 §6 |
| `kUIPropAutoStartOutput` | `"ui_prop_auto_start_output"` | BOOL | OBS 前端自管的开机自启 | **本插件不用**（生命周期自管，reconcile 驱动） |

**联动关系**（三个 modified 回调）：
- `aja_output_device_changed`（`aja-output.cpp:800-857`）：重填 IOSelection / videoFormat / pixelFormat / SDITransport / SDITransport4K 列表；末尾 `update_sdi_transport_and_sdi_transport_4k`（`:66-80`）设两个 SDI 项的可见性。
- `aja_output_dest_changed`（`:859-908`）：若所选 IOSelection 已被占用则回退到 `Invalid`（`:891-901`）；再 `update_sdi_transport_and_sdi_transport_4k`。
- `aja_video_format_changed`（`aja-common.cpp:216-239`）：videoFormat 不在列表时插禁用占位项；`SDITransport4K` 可见性 = `NTV2_IS_4K_VIDEO_FORMAT`。
- `update_sdi_transport_and_sdi_transport_4k`（`:66-80`）：`SDITransport` 可见 ⇔ `IsIOSelectionSDI(io)`；`SDITransport4K` 可见 ⇔ SDI 且 4K 格式。

### 1.3 create 前置条件与失败点（`aja_output_create` 934-1013）
读 settings → `OutputProps`，随即校验，任一失败 `return nullptr`（**均安全，无崩溃**）：
- cardID 空（`:939`）；CardEntry / card 不存在（`:947-956`）；
- `ioSelect == IOSelection::Invalid`（`:969`）；
- `videoFormat == NTV2_FORMAT_UNKNOWN` 或 `pixelFormat == NTV2_FBF_INVALID`（`:973`）；
- 该 IOSelection 无输出目的地（`:982`）；
- `AcquireOutputSelection(ioSelect, deviceID, outputID)` 失败（通道被别的 owner 占用，`:988`）。
成功后：`new AJAOutput` → `Initialize(props)`（`:995`，DMA 黑帧 + 等垂直中断 + 算帧率）→ `ClearVideoQueue/ClearAudioQueue` → `SetOBSOutput` → **`CreateThread(true)` 立即起输出线程**（`:999`）。

### 1.4 start（`aja_output_start` 1022-1110）
- 无 fps 硬查（对比 DeckLink）。多 framestore 时同步各通道 videoFormat/pixelFormat（`:1060-1070`）；`ConfigureOutputRoute` 配置 crosspoint（失败 `return false`，`:1075`）；`ConfigureOutputAudio`。
- **视频转换**（承重）：
  ```
  scaler.format     = AJAPixelFormatToOBSVideoFormat(pixelFormat);   // :1084 UYVY 或 BGR3
  scaler.width      = FormatDesc().GetRasterWidth();                 // :1085 光栅宽
  scaler.height     = FormatDesc().GetRasterHeight();                // :1086 光栅高
  scaler.colorspace = VIDEO_CS_709;  scaler.range = VIDEO_RANGE_PARTIAL; // :1090-1091
  obs_output_set_video_conversion(output, &scaler);                  // :1093
  ```
  紧邻此处有 AJA 自己的 TODO：「The colors are off when outputting the frames that OBS sends us」（`:1087-1089`）——**已知色彩风险，非本插件引入**，见 §8。
- **音频转换**（`:1095-1100`）：format=`AUDIO_FORMAT_32BIT`、speakers=`SpeakerLayout()`（`OutputProps` 默认 8ch → `SPEAKERS_7POINT1`，`aja-props.cpp:228,394-401`）、samples=48000。
- `obs_output_begin_data_capture(output, 0)` 失败则 `return false`（`:1102`）。

### 1.5 raster / pixelFormat → OBS 格式映射 + audio
- 光栅字节：`raw_video` 按 `FormatDesc().GetTotalRasterBytes()` 入队（`aja-output.cpp:1162`）——**OBS 转换后的帧尺寸必须等于光栅**，否则越界/花屏。因 `start` 里 conversion 的 w/h 即光栅，libobs 缩放器保证输出光栅尺寸（见 §3）。
- `AJAPixelFormatToOBSVideoFormat`（`aja-common.cpp:243-293`）：`NTV2_FBF_8BIT_YCBCR → VIDEO_FORMAT_UYVY`（`:247-249`）、`NTV2_FBF_24BIT_BGR → VIDEO_FORMAT_BGR3`（`:250-252`）。其余多数 → `VIDEO_FORMAT_NONE`（但输出列表只放这两种，无碍）。
- audio：NTV2 恒 32-bit PCM（`aja-props.cpp:388-392`）；默认 8ch/7.1/48k。

### 1.6 帧率：软对齐，非硬查
`Initialize`（`:173-178`）从 videoFormat 取固定 fps（`GetNTV2FrameRateFromVideoFormat` + `GetFramesPerSecond`）。画布 fps 与之不符时，`OutputThread` 用 `mVideoAdjust`（丢帧/重帧，`:722-742`）软对齐——**不失败但 A/V 会漂移**。规避手段已内建：`aja_output_device_changed` 调 `populate_video_format_list(..., matchFPS=MATCH_OBS_FRAMERATE)`（`aja-output.cpp:25` 定义为 `true`，`:845` 传入），其内 `obs_get_video_info` 取画布 fps，逐格式比 `obsFrameTime != ajaFrameTime` 则剔除（`aja-common.cpp:158-168`）。⇒ **对话框看到的 videoFormat 列表天然只含匹配画布 fps 者**，本插件复用即可（见 §4）。

### 1.7 stop（`aja_output_stop` 1112-1152）与 destroy（`aja_output_destroy` 910-932）
- stop：`ReleaseOutputSelection(ioSelect, deviceID, outputID)`（`:1138`，owner 串必须与 create 时一致）→ 写黑帧 → `obs_output_end_data_capture` → `StopAudioOutput` → `ClearConnections`。
- destroy：`StopThread()`（join 输出线程，`:927`）→ 清队列 → `delete`。
- `update`（`:1015-1020`）是**空实现**——运行中改设置无效，配置变更必须走「stop + 重建」。

---

## 2. AjaBackendSettings 形状（对标 `DeckLinkBackendSettings`）

新增独立结构（不污染共享 `OutputBackendSettings`），置于 `multiview-instance.hpp`（对标 `:715-723`）：

```cpp
struct AjaBackendSettings {
    std::string cardID;      // kUIPropDevice（STRING，"设备ID串_序列号"）
    long long   ioSelect      = (long long)IOSelectionInvalid; // 22，见下
    long long   videoFormat   = 0;   // NTV2VideoFormat，0 = NTV2_FORMAT_UNKNOWN
    long long   pixelFormat   = 0;   // NTV2PixelFormat，0 起需校验为有效
    long long   sdiTransport  = 0;   // SDITransport（SingleLink=0）
    long long   sdi4kTransport= 1;   // SDITransport4K（2SI=1）
    // 注意：不持久化 outputID —— 每次 create 现生成保证唯一（见 §6）
    obs_data_t *to_obs_data() const;
    static AjaBackendSettings from_obs_data(obs_data_t *data);
};
```

- **零 SDK 表达**：所有 AJA 枚举以 `long long` 原样透传（就是 `obs_data_get_int` 读出的整型），不引 NTV2 头。哨兵常量（`IOSelection::Invalid=22`、`NTV2_FORMAT_UNKNOWN=0`）在本插件内以命名常量复刻并注释来源（`aja-enums.hpp:46`、SDK 定义）；`NTV2_FBF_INVALID` 无稳定小整数保证，故 pixelFormat 的「有效」判定不写死某值，而是**以「是否命中对话框枚举出的合法列表」为准**（见校验）。
- **校验/钳位（对标 H2，但 AJA 更安全——无效值不会崩，只是建不出）**：`from_obs_data` 用 `obs_data_has_user_value` 守卫；负值一律钳到「未设」哨兵。真正的「拒建」在后端 create 任务里（§5）判定：`cardID` 非空、`ioSelect != Invalid`、`videoFormat != UNKNOWN`、`pixelFormat` 合法，否则不投递 create（并进冷却，避免刷屏），**绝不**拿明显无效值去 `obs_output_create`（虽不会崩，但白占卡 + 刷日志）。
- 复用共享 `OutputBackendSettings` 的 `enabled` / `audioMode` / `audioTrackIndex`；`resMode`/`fpsDivisor` 见 §3、§4。
- **序列化 nest 键**：`"ajaHw"`（对标 DeckLink 的 `"decklinkHw"`，见 §5）。

---

## 3. 喂帧方案（承重：与 DeckLink 本质不同）

### 3.1 结论：喂画布尺寸 BGRA，交 libobs 缩放器缩到光栅 + 转格式
- 我们 compose 出 `GS_BGRA` texrender → `StagedReadback` 双缓冲回读 BGRA（`multiview-output-staged-readback.hpp`，与 DeckLink/NDI 同一份）。
- `video_t` 以 **BGRA、画布尺寸** 打开（不是光栅——**AJA 无法在建 video_t 前拿到光栅**，见 §0 差异表）。
- `aja_output_start` 会把 conversion 设成 `{UYVY|BGR3, 光栅 w/h, 709, PARTIAL}`（`aja-output.cpp:1093`）。
- **libobs 原始视频路径**在 conversion 的 `width/height/format/range/colorspace` 与源 `video_t` 不一致时，`video_input_init` 用 `video_scaler_create(..., VIDEO_SCALE_FAST_BILINEAR)` 建缩放器（`video-io.c:342-366`），**同时完成分辨率缩放 + 像素格式/色域/范围转换**，产出恰为光栅尺寸的目标格式帧。`start_raw_video` 传入的正是 `obs_output_get_video_conversion(output)`（`obs-output.c:2520`）。
- 因此 `raw_video` 读 `GetTotalRasterBytes()`（`aja-output.cpp:1162`）与缩放器产出尺寸一致，**无越界**。
- **可行性铁证**：OBS 原生 aja-output-ui 的预览输出正是此法——`video_t` 开在**画布 base_width/height** 的 BGRA（`aja-ui-main.cpp:127-160`：texrender/stagesurface `GS_BGRA`、`vi.format=VIDEO_FORMAT_BGRA`、`obs_output_set_media(output, video_queue, obs_get_audio())`）。

### 3.2 video_t 参数
```
voi.format     = VIDEO_FORMAT_BGRA;
voi.width/height = 合成尺寸（默认画布 base_width/base_height，见 §4）;
voi.fps_num/den  = ovi.fps_num/den;
voi.cache_size = 4;
voi.colorspace = VIDEO_CS_709;      // 与 aja start 的 conversion 对齐
voi.range      = VIDEO_RANGE_FULL;  // BGRA 全范围；conversion 目标为 PARTIAL，缩放器负责范围转换
voi.name       = "amv-aja-<uid>"（backing string 随 OutputInstance 存活，libobs 只存指针）;
```

### 3.3 pixelFormat 范围与色彩风险（推荐见 §9）
- 输出**只有** UYVY / BGR3 两种，**都不是 BGRA**，故喂帧必经一次转换——**没有「BGRA 友好子集」可退守**，restrict 无意义。
- AJA 自带的 `// colors are off` TODO（`aja-output.cpp:1087-1089`）说明其 conversion 的色域/范围处理已知有瑕。本插件**照原样接线**，把它列为**已知风险（真机待验）**，不试图在无硬件时「修」它。

### 3.4 compose_size（M1）：AJA 返回 false
DeckLink 用 M1 把合成尺寸锁到光栅（因它能拿到光栅）。**AJA 拿不到光栅**（`create` 后 `get_video_conversion` 返回 NULL），故 `compose_size` 返回 `false`（默认实现，`multiview-output.hpp:92-97`），manager 回落 `resolve_output_dimensions`（画布/自定义）。可选增强（P3）：`obs_output_start` 成功后读一次 `obs_output_get_video_conversion(out)` 仅用于**日志**记录实际光栅、当与合成尺寸差异很大时告警（缩放开销/画质提示），**不**改喂帧尺寸。

---

## 4. 对话框（完整控制）

新增 AJA 页（对标 DeckLink 页 `external-output-settings-dialog.cpp:248-333`）。控件：`enabled` + **device / IOSelection / videoFormat / pixelFormat / SDITransport / SDITransport4K** + 音频（audioMode + audioTrack，`supportsAudio=true`）。**不含** resMode/custom/fps（合成默认画布；理由见下），或如需高级可选放开 resMode 但 `fpsDivisor` 恒 1。

### 4.1 无 SDK 驱动 aja_output 属性枚举（对标 `populate_decklink_modes`）
`obs_get_output_properties("aja_output")`（未注册返回 NULL → 整页灰置）取各属性，用 scratch `obs_data` + `obs_property_modified` 触发 OBS 自己的回调来填列表，我们只读结果项：
1. **设备**：读 `kUIPropDevice` 列表项（`obs_property_list_item_count/name/string`，data=cardID 串）填设备下拉。
2. **设备变更**：scratch 写入 `kUIPropDevice` + `kUIPropAJAOutputID`（临时串）→ `obs_property_modified(deviceProp, settings)` 触发 `aja_output_device_changed`，它填好 IOSelection / videoFormat（**已按画布 fps 过滤**）/ pixelFormat / SDITransport / SDITransport4K 五个列表；逐一读项填 Qt 下拉（IOSelection/format 用 `obs_property_list_item_int` 取整值，pixel 同）。
3. **视图联动可见性（零 SDK）**：不自己判 `IsIOSelectionSDI`/`4K`（那要 SDK），而是——把当前 `kUIPropOutput`(IOSelection) 与 `kUIPropVideoFormatSelect` 写入 scratch，触发对应回调后**读 `obs_property_visible(sdi_trx)` / `obs_property_visible(sdi_4k_trx)`**，镜像到 Qt 控件的显隐。即让 OBS 的 `update_sdi_transport_and_sdi_transport_4k`（`aja-output.cpp:66-80`）替我们算。
   - IOSelection 变 → 触发 `aja_output_dest_changed`（`:859`）→ 读两个 SDI 可见性。
   - videoFormat 变 → 触发 `aja_video_format_changed`（`aja-common.cpp:216`）→ 读 SDITransport4K 可见性。
4. `obs_properties_destroy` 释放。

### 4.2 videoFormat 按画布 fps 过滤
**无需自实现**：`aja_output_device_changed` 已用 `matchFPS=MATCH_OBS_FRAMERATE(true)` 调 `populate_video_format_list`（§1.6），列表天然只含匹配画布 fps 的格式。对话框直接读该列表即可。可加一条本地化提示：「仅列出与画布帧率匹配的视频格式」（对标 DeckLink 的 `FpsNote`）。

### 4.3 load/read（对标 `load_decklink`/`read_decklink`）
- load：按 cardID 选设备 → 触发填 IOSelection/format 列表 → 按保存值 `findData` 选中（缺则留首项/Select…）→ 读 SDI 可见性。
- read：收集六个 AJA 旋钮 + enabled + 音频到 `AjaBackendSettings`/`OutputBackendSettings`。**拒绝落盘 enabled + 无效选择**（对标 H2）：`enabled && (cardID 空 || ioSelect==Invalid || videoFormat==UNKNOWN || pixelFormat 非法) → enabled=false`。**AJA 无需 DeckLink 的探针 create**（探针拿不到光栅，且会占卡起线程，代价高、收益无）。

---

## 5. 接线（对标 DeckLink，但落到 A1/A2/A3 现行注册表架构）

> DeckLink 设计文档 §4 的「加 `decklink_` 成员 + `entries[]` 三数组」是**旧架构**，已被 A1/A2/A3 重构取代。以下以**现行**代码为准（我已读 `multiview-output.cpp/.hpp` 现状）。

1. **枚举**：`multiview-instance.hpp:687` `enum class OutputBackendKind { Spout, Ndi, Decklink, Aja };` 追加 `Aja`。
2. **注册表描述符**（`multiview-output.cpp:85-111` `output_backend_registry()`）：`#ifdef AMV_ENABLE_AJA_OUTPUT` 下 `r.push_back({OutputBackendKind::Aja, "aja", "AJA", &MultiviewOutputManager::aja_supported, &create_aja_output_backend, /*supportsAudio=*/true});`。
3. **available 门控**：`multiview-output.hpp` 声明 + `.cpp` 实现 `static bool aja_supported()` = `#ifdef AMV_ENABLE_AJA_OUTPUT` 且 `obs_get_output_flags("aja_output") != 0`（对标 `decklink_supported()` `:73-83`）。因 aja 无卡不注册（§1.1），此门天然含「有卡」语义。
4. **序列化**（`multiview-instance-serialize-output.cpp`）：
   - 共享 `OutputBackendSettings` 已按注册表遍历自动读写 `"aja"` 子对象（`:222-302`，**零额外代码**）。
   - `AjaBackendSettings::to/from_obs_data` 新增（对标 DeckLink `:177-209`），`InstanceOutputSettings` 加成员 `AjaBackendSettings aja;`（always-present，独立于构建开关，保证配置无损往返），`to/from_obs_data` 加 `"ajaHw"` 键（对标 `"decklinkHw"` `:236-237,265-267`）。
   - `unknownBackends` 机制天然覆盖「本构建无 aja 时保留他机 aja 配置」——`consumed` 集合加 `"ajaHw"`（对标 `:290`）。
   - **不**照搬 DeckLink 的「force resMode=Custom」（那是为锁光栅；AJA 合成画布，`resMode` 保持默认 CanvasBase）。
5. **接口通道**：`IMultiviewOutputBackend` 加 `virtual void configure_aja(const AjaBackendSettings &hw) {}`（对标 `configure_decklink` `multiview-output.hpp:79`）。`reconcile`（`multiview-output.cpp:129-172`）在 `configure_audio` **之前**、无条件对每个后端调 `e.backend->configure_aja(cfg.aja);`（对标 `configure_decklink` `:168`），非 AJA 后端默认空实现。
6. **后端**：新增 `src/multiview-output-aja.hpp/.cpp`，`AjaOutputBackend : IMultiviewOutputBackend` + 工厂 `create_aja_output_backend()`，**以 `multiview-output-decklink.cpp` 为模板**（SharedState + `DeckState`→`AjaState`、UI 线程生命周期 H1、`StagedReadback` A5、cooldown、liveness self-heal M2）。差异见 §7。
7. **S1 armed 计数**：`plugin-main.cpp:176-178` 遍历 `os.backends` 累加 `enabled`——AJA 作为注册表新 kind 天然计入，无需改（`multiview_refresh_output_driver` `:157-206`）。
8. **CMake**（`CMakeLists.txt:159-163` 紧邻 DECKLINK）：`option(ENABLE_AJA_OUTPUT "Build multiview AJA output (reuses OBS aja_output)" ON)` → `target_compile_definitions(... AMV_ENABLE_AJA_OUTPUT)`，新 .cpp 入源列表（guard 下）。**纯 libobs，无 NTV2 SDK 查找/链接**。
9. **本地化**：`data/locale/en-US.ini` + `zh-CN.ini` 加 `AMVPlugin.Output.Tab.AJA` 及 device/IOSelection/videoFormat/pixelFormat/SDITransport/SDITransport4K 标签、fps 提示、Unavailable 文案（对标 DeckLink 键 `:154-177`）。**两文件 key 集合必须一致**（deploy 脚本会拦）。

---

## 6. OutputID 唯一性（对标 X1）

### 事实（`aja-card-manager.cpp`）
CardManager 单例（在 OBS **aja 模块**内，进程级、跨我方所有实例 + OBS 自带 aja 输出/采集源共享）用 `mChannelPwnz = map<owner串, 通道位掩码>` 跟踪通道占用：
- `ChannelReady(chan, owner)`（`:178-188`）：某通道被占时，**仅当占用者 owner 相同才返回 true**。
- `AcquireOutputSelection(io, id, owner)`（`:356-408`）：逐通道 `AcquireChannel`，任一拿不全则回滚、返回 false → `aja_output_create` 返回 nullptr（`:988`）。

### 推论（承重）
- **不同** owner 争**同**通道 → 第二者 `ChannelReady` 假 → acquire 失败 → 建不出 → 我方冷却重试。**正确互斥**。
- **相同** owner 争**同**通道 → `ChannelReady` 真（owner 匹配）→ 第二者「以为」也拥有该通道 → **两路同占一通道 = 硬件冲突/UB**。
- OBS 自带只用两个固定串 `kProgramOutputID="aja_output"` / `kPreviewOutputID="aja_preview_output"`（`aja-ui-props.hpp:5-6`，在 `AJAOutputUI.cpp:62,111` 写入 settings），天然两路不冲突。**我方多实例/多路必须各自唯一**。

### 推荐方案
- 每个后端实例在 **create 任务**里现生成唯一 owner 串：`"amv-aja-" + <进程级 atomic 计数器>`（对标 DeckLink `next_uid()` `multiview-output-decklink.cpp:614-618`），可再拼实例 uuid 便于日志：`"amv-aja-" + instanceUuid + "-" + n`。存入 `OutputInstance`（随其存活），`create`/`stop` 用**同一串**（release 按同 owner）。
- **不持久化**：每会话现生成，杜绝「同配置克隆到两实例」或「重启后残留 owner」的碰撞。
- `"amv-aja-"` 前缀确保永不撞 OBS 的 `aja_output`/`aja_preview_output` 及采集源 owner。

---

## 7. 不变式红线（照 DeckLink 硬化经验，落到 AJA）

- **H1 UI 线程生命周期**：`aja_output` 的 create（占卡 + 等垂直中断 + 起线程）与 destroy（join 线程）都重且同步——**全部经 UI 线程**（DeckLink 用 `QMetaObject::invokeMethod(qApp, …, Qt::QueuedConnection)` 投递，`multiview-output-decklink.cpp:493-500,545-550`；create 走 `obs_queue_task(OBS_TASK_UI,…)` `:500`）。图形线程 `submit_frame` 只做 stage/map/memcpy。
  - **AJA 特有**：重活在 **create**（不像 DeckLink 集中在 start/stop），故 create 任务必须在 UI 线程；被 cancel 时按 `close_output_instance` 序拆解：`obs_output_stop → obs_output_release`（触发 `aja_output_stop` 释放 IOSelection + `aja_output_destroy` join 线程）→ `video_output_close` →（None 模式）`audio_output_close`（对标 `:113-127`）。
- **A5 StagedReadback 复用**：直接复用 `multiview-output-staged-readback.hpp`，尾部 `feed_frame` 逐行 memcpy 进 video_t（对标 DeckLink `:580-612`，持 `sh_->mtx` 跨 lock_frame→memcpy→unlock_frame）。
- **M1 compose_size**：AJA 返回 false（§3.4），manager 用 `resolve_output_dimensions`（画布）。**不**照搬 DeckLink 的光栅锁定。
- **图形线程契约**：`stop()` 可能在图形线程或主线程持图形锁时被调（`multiview-output.hpp:99-125`）——重拆解一律异步投递，绝不 inline join。`obs_frontend_*` 只经主线程缓存（`amv_frontend::streaming_mixers()`）读，绝不在图形/UI-任务直呼。
- **可用性灰置兜底**：`aja_supported()` 假 → 对话框整页灰置 + 原因文案；`enabled` 变安全 no-op；无卡时 `obs_get_output_properties("aja_output")` 返回 NULL，枚举全空。**这是测试后置的核心兜底**（绝大多数机器无 aja_output 注册，AJA 页恒灰置，零风险）。
- **M2 liveness self-heal**：输出在底层死掉（卡被拔/错误停）时 `obs_output_active` 转假，照 DeckLink 检测并拆重建（`multiview-output-decklink.cpp:427-447`）。
- **配置钳位**：复用 `config-limits.hpp` 的 `kMinDim/kMaxDim` 钳合成尺寸；AJA 枚举值以「命中合法列表」判定而非硬钳（§2）。

---

## 8. 风险 & 验证

### 风险
- **无硬件 → 测试后置**：本机无 AJA 卡且标准 OBS 未含 aja 插件，`aja_output` 不注册，发送链路无法真机验证。靠可用性灰置兜底先实现+提交（§7）。
- **色彩偏差（已知）**：AJA 自带 `// colors are off` TODO（`aja-output.cpp:1087-1089`）+ 我方 BGRA→UYVY/BGR3 + 709/FULL→PARTIAL 缩放器转换，色域/范围可能有偏。**真机待验**，可切 UYVY↔BGR3 对比。
- **fps 错配**：画布 fps 与 videoFormat 不符时不失败但 A/V 漂移（§1.6）。已靠 fps 过滤列表规避；仍需真机确认切换画布 fps 后旧配置失效的表现（列表变空/该 videoFormat 被禁用占位）。
- **start 失败面**：IOSelection 被占（多路/多实例/OBS 自带输出/外部 App）→ create 返回 nullptr → 冷却重试 + 记 owner 与 IOSelection（对标 DeckLink X1 日志 `:356-359`）。
- **缩放画质**：画布尺寸 ≠ 光栅时 libobs FAST_BILINEAR 缩放（`video-io.c:352`）。画布与目标 SDI 光栅一致（如 1080p→1080p）近似零缩放；差异大则有软缩放开销/画质损。

### 真机待验清单（对标 DeckLink §D / §7.3）
1. `obs_get_output_flags("aja_output")!=0`、设备/IOSelection/videoFormat/pixelFormat/SDITransport/SDITransport4K 枚举正确、fps 过滤生效。
2. 选定配置 → SDI/HDMI 正确出多画面；切 pixelFormat（UYVY/BGR3）看色彩；切 IOSelection/videoFormat 看 SDITransport/4K 显隐联动。
3. 音频三模式（Follow/Manual/None）；None 静音路径不致欠载。
4. 反复启用/禁用/关窗/切场景集合 → OBS 绝不崩、主节目输出无 hitch。
5. 多实例/多路：各 owner 唯一、IOSelection 冲突时后者优雅失败（不崩、有日志）。
6. 改画布 fps 使 videoFormat 失效后的表现；AJA + DeckLink + NDI + Spout 并存压测。

---

## 9. 两处推荐方案与理由

### 9.1 喂帧格式 / pixelFormat 范围
**推荐：两种像素格式全开，默认 8-bit YCbCr（UYVY）；喂 BGRA@画布尺寸，交 libobs 缩放器缩到光栅 + 转格式。**
- 理由：① 输出只有 UYVY / BGR3，无 BGRA，无「友好子集」可退，restrict 无意义；② UYVY 是 OBS aja 默认（`kDefaultAJAPixelFormat`，`aja-common.cpp:22`）、SDI 原生、带宽为 BGR3 一半、广播标准；③ 保留 BGR3 供真机对比色彩（应对已知 `colors are off`）；④ 画布尺寸喂帧 = OBS 原生 aja 预览的既验路径（`aja-ui-main.cpp:127-160`），无需光栅、无需 SDK 表、`create` 前无从拿光栅（§0 差异表）。
- 「完整控制」达成：六个 AJA 硬件旋钮全暴露，pixelFormat 用户自选，仅默认值给 UYVY。

### 9.2 OutputID 生成
**推荐：每后端实例 create 时现生成 `"amv-aja-" + 进程级 atomic 计数器`（可拼实例 uuid），不持久化，create/stop 复用同串。**
- 理由：CardManager 以 owner 串判通道占用，相同 owner 会误判「同占」致硬件冲突（§6 推论）；唯一 owner 才有正确互斥。现生成杜绝克隆/重启碰撞；`amv-aja-` 前缀避开 OBS 自带 `aja_output`/`aja_preview_output`。对标 DeckLink `next_uid()`（`multiview-output-decklink.cpp:614-618`），但 AJA 的唯一性是**功能正确性硬要求**（DeckLink 靠 device_hash 天然区分，AJA 无此天然键）。

---

## 10. 明确不做 / 暂缓（分阶段）

- **P1**：后端 + 注册表接线 + `AjaBackendSettings` 序列化（最小 UI 即可先出画）；HD/单链路 SDI（IOSelection=SDI1…SDI8 单路）优先，UYVY 默认；启停稳定、主节目无 hitch。
- **P2**：完整对话框（六旋钮枚举 + fps 过滤 + SDI/4K 显隐联动 + 音频）、配置往返持久化。
- **P3 硬化**：UI-线程生命周期与 video_ 跨线程竞态审计、画布 fps 变更致格式失效的校验/提示、尺寸变更重开、None 静音实测、M2 self-heal、AJA+DeckLink+NDI+Spout 并存压测；沉淀 `docs/issue-18-aja-output-hardening-notes.md`。
- **暂缓（复杂度过大时标注分阶段）**：4K（SDITransport4K Squares/2SI、多线路 IOSelection 如 SDI1-4/SDI5-8）与多路并发的真机联调——依赖真机验证，无卡不落地；先保证 HD 单链路稳，4K 作为 P3+ 独立小阶段。12G 输出本被 OBS 禁用（`aja-common.cpp:200`），不涉及。
```

