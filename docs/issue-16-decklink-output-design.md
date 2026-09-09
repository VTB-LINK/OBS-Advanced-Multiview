# Issue #16 — BMD DeckLink 输出设计文档

状态：设计定稿，待实现
范围：**仅 DeckLink**（AJA 留作后续独立阶段，见 §8）
关联：issue #11 外部输出层（Spout/NDI）、`docs/` 既有硬化笔记

---

## 1. 动机

issue #16：OBS 已内置输出到 BMD DeckLink / AJA 硬件接口的能力；本插件的多画面（multiview）应支持把合成画面经 **DeckLink SDI/HDMI** 输出，与现有 NDI / Spout 输出并列，作为广播级硬件监看 / 下游送播信号。

经探索 obs-studio 源码与本插件输出层，确定**不自接 DeckLink SDK**，而是**复用 OBS 已注册的 `decklink_output` 输出类型**，把多画面自渲染帧经自建 `video_t` 喂给它。本插件已把多画面合成进 `GS_BGRA` 的 `gs_texrender`，NDI 后端已实现双缓冲 `gs_stagesurface` GPU→CPU 回读——新后端复用该回读，尾部改为写入 `video_t`。

---

## 2. 约束（先过 §0 四条）

1. **绝不炸 OBS**：`decklink_output` 的停止是**异步**的，释放时会 join 数据捕获线程并可能 Deactivate 硬件；`video_t` 被该输出的捕获线程与本插件图形线程双向访问。错误的销毁顺序 = use-after-free。
2. **非阻塞**：`obs_output_release`→`obs_output_destroy` 内部 `os_event_wait(stopping_event)` + `pthread_join(end_data_capture_thread)` + DeckLink `Deactivate()`，同步耗时数~十 ms。**绝不能在 OBS graphics 线程上同步执行**（会 hitch 主节目输出）。
3. **非入侵 / 隔离**：DeckLink 输出失败（设备占用、FPS 不匹配）只降级为「不出画 + 日志」，不连累其它后端 / 实例 / OBS 本体。
4. **广播级稳定**：环满丢帧、启动失败冷却重试、拆解幂等、反复启停不崩不卡。

### 关键外部事实（均已读源码核实，勿凭记忆）
- `decklink_output`（`obs-studio/plugins/decklink/decklink-output.cpp:280`）：`flags = OBS_OUTPUT_AV`（原始音视频）；`create` 内 `obs_output_set_video_conversion(output, {VIDEO_FORMAT_BGRA, 模式宽高, FULL, 709/2100_PQ})`——**输出自行把我们的 video_t 转成 BGRA 并对齐模式光栅**，喂 BGRA@光栅即零转换。
- 设置键（`plugins/decklink/const.h`）：`device_hash`(string) / `mode_id`(int) / `keyer`(int 0/1/2) / `force_sdr`(bool)。`auto_start` 由前端自管、不用。
- **FPS 严格相等**（`decklink-output.cpp:94`，`DeckLinkDeviceMode::IsEqualFrameRate`）：`start` 要求所选模式帧率 == **全局画布帧率**，否则「FPS mismatch」失败。模式下拉（`:226-232`）也只列 `IsEqualFrameRate` 为真者。故本后端**不提供半率**（连续 SDI 必须每帧喂，半率会饿死输出）。
- **音频强制**：`OBS_OUTPUT_AV` 的 `can_begin_data_capture` 要求 `output->audio != NULL`；`decklink_output_start` 还要求 `obs_get_audio_info()` 有效（全局音频须初始化）。
- **销毁同步且安全**：`obs_output_destroy`（`obs-output.c:291`）→ `obs_output_actual_stop(force)` → `os_event_wait` + `pthread_join(end_data_capture_thread)`；`end_data_capture_thread`（`:2837`）内 `stop_raw_video` 断开我们的 video_t。⇒ `release` 返回后已无人再读 video_t，之后 `video_output_close` 安全。
- 线程投递：libobs 有 `obs_queue_task(OBS_TASK_UI, fn, param, wait)`（`obs.h:932`）；本插件也惯用 `QTimer::singleShot(0, core, …)`。
- 承重 API 全部存在：`obs_get_output_flags` / `obs_get_output_properties` / `obs_output_set_media` / `obs_output_set_mixer` / `obs_output_get_video_conversion`（返回含模式宽高的 `video_scale_info`）/ `video_output_open·lock_frame·unlock_frame·close` / `audio_output_open` / `obs_property_modified` · `obs_property_list_item_string/int/count`。

---

## 3. 方案

### 3.1 总体
新增后端 `DeckLinkOutputBackend : IMultiviewOutputBackend`（`multiview-output.hpp:29` 的既有抽象），接入 `MultiviewOutputManager` 的第三个 `Kind::Decklink` 槽，与 Spout/NDI 并列。复用 NDI 后端（`multiview-output-ndi.cpp`）的「双缓冲 stagesurface 回读」骨架，尾部由「推 NDI」换成「写 `video_t`」。

### 3.2 线程模型（崩溃面核心，务必照此实现）
- **图形线程**（`submit_frame`，manager 渲染回调内）：只做快速且非阻塞的工作——`ensure_stage` / `gs_stage_texture` / `gs_stagesurface_map` / `video_output_lock_frame`+逐行 memcpy+`unlock_frame` / `gs_stagesurface_unmap`。**绝不**在此调用 `obs_output_create/start/stop/release` 或 `video_output_open/close`。
- **UI 线程**（经 `obs_queue_task(OBS_TASK_UI, …, /*wait=*/false)`）：执行所有 output / video_t / 硬件生命周期（create/start/stop/release/close）。理由见 §2 约束 2。
- **`video_` 指针跨线程**：UI 线程建/关，图形线程读。用一把 `std::mutex mtx_` 守 `video_` 的可见性：
  - `submit_frame`：`lock; if(!video_){unlock;return;} lock_frame→memcpy→unlock_frame; unlock;`（临界区含 memcpy，保证 close 不会在 lock_frame 半途发生）。
  - 拆解：① `lock; vq=video_; video_=nullptr; unlock;`（图形侧即刻停手）→ ② `obs_output_stop(out); obs_output_release(out);`（**锁外**，停输出侧并 join，慢但在 UI 线程）→ ③ `video_output_close(vq);`（此刻图形已停、输出已 join，安全）。
  - `active_` 用 `std::atomic<bool>`。

### 3.3 生命周期
状态：`Idle → Starting(UI 任务在途) → Running → Stopping(UI 任务在途) → Idle`，用原子/标志表达，避免重复投递。

- `configure_audio(cfg)`（图形线程，reconcile 每帧调，复用此入口承接全部 DeckLink 配置）：缓存 `deckDeviceHash/deckModeId/deckKeyer/deckForceSdr/audioMode/audioTrackIndex`。与运行中配置比较；Idle 且 enabled→投递 **create 任务**；Running 且关键项变化→投递 **teardown 任务** 并置 Idle（下一帧再触发 create）。用 `pending_` 标志防重复投递。
- **create 任务**（UI 线程）：`obs_get_output_flags("decklink_output")==0`→放弃（标记不可用）。组 `obs_data`（device_hash/mode_id/keyer/force_sdr）→ `obs_output_create("decklink_output", <实例名>, settings, NULL)`；读 `obs_output_get_video_conversion(out)` 确认光栅 `w/h`（为 null 说明设备/模式无效→release 放弃）；`video_output_open(&vq, {format=BGRA, width/height=光栅, fps_num/den=全局 ovi, cache_size=4, colorspace=VIDEO_CS_709, range=VIDEO_RANGE_FULL, name="amv-decklink-<uuid>"})`；解析音频 audio_t（§3.5）；`obs_output_set_media(out, vq, audio_t)`；ManualTrack 时 `obs_output_set_mixer(out, track-1)`；`obs_output_start(out)`。成功：`lock; video_=vq; out_=out; unlock; active_=true`。失败：逆序拆解 + 记日志 + 设冷却（避免每帧重试刷屏），置 Idle。
- `prepare(name)`（图形线程）：轻量——仅在未创建且 enabled 且冷却到期时触发 create 投递（也可合并进 configure_audio 的判断）。**不**在此做重活。
- `wants_frame()`：`return active_`（SDI 需连续喂帧，始终要帧）。
- `submit_frame(name, tex, w, h, _)`：未 active 直接返回。`ensure_stage(w,h)`（2× `gs_stagesurface_create(GS_BGRA)`，尺寸变→重建，并触发 output 重建以匹配新光栅）。双缓冲 ping-pong：`gs_stage_texture(cur)` 本帧，映射上一帧的 prev 面。`lock mtx_; if(video_){ if(video_output_lock_frame(video_,&f,1,os_gettime_ns())){ 逐行 memcpy（按 f.linesize[0] 与 mapped linesize，二者可能不等）; video_output_unlock_frame(video_);} else 丢帧（环满，不阻塞）} unlock;`。`gs_stagesurface_unmap`。回读骨架逐字参考 `multiview-output-ndi.cpp:74-133, 291-317`。
- `stop()`（图形线程，reconcile 禁用 / teardown_locked / 析构调用；幂等、未启动也安全）：`destroy_stages()`（图形资源，本线程直接销毁）；`lock; vq=video_; video_=nullptr; out=out_; out_=nullptr; active_=false; unlock;`；若 `out`/`vq` 非空→投递 **teardown 任务**（见 §3.2 ②③，持有 out+vq+silent_audio 所有权，任务自删）。对象可在 stop() 后被立即 `reset()`（句柄已移出，无悬挂）。
- **teardown 任务**（UI 线程）：`obs_output_stop(out); obs_output_release(out); video_output_close(vq); if(silent_audio) audio_output_close(silent_audio);`。

### 3.4 分辨率：锁定模式光栅（无 SDK）
对话框选定「设备+模式」后，临时 `obs_output_create("decklink_output",…)` 并读 `obs_output_get_video_conversion(tmp)->width/height` 得精确光栅，写入 `OutputBackendSettings.customWidth/customHeight`，随即 release 临时 output。运行时 `resolve_output_dimensions`（`multiview-instance-serialize-output.cpp:201`，Custom 分支返回 `{customWidth, customHeight}`）⇒ manager 合成即 SDI 光栅，**无需改分辨率解析逻辑**。后端 create 时再以 `obs_output_get_video_conversion` 复核一次、以其为准开 video_t。

### 3.5 音频（复用 `OutputAudioMode`）
`decklink_output` 经自身 `raw_audio` 回调从 set_media 的 audio_t + 输出 mixer 拉音频：
- `ManualTrack(n)`：`obs_get_audio()` + `obs_output_set_mixer(out, n-1)`。
- `FollowStreaming`：`obs_get_audio()`，mixer 取流输出音轨——经**主线程更新的 frontend 缓存**（参考 NDI `amv_frontend::streaming_mixers()`，**绝不在图形/UI-任务内直接问 obs_frontend**；而 create 任务在 UI 线程，可安全读缓存）；缺省 track 1 / mixer 0。
- `None`：`audio_output_open` 建**静音 audio_t** 传入（AV 输出要求 audio 非空；SDI 走数字静音）。静音实现 = 建一个从不被喂数据 / 固定静音的 audio_t。**需实测**不致输出音频欠载（列为验证项）。

### 3.6 可用性门控
`MultiviewOutputManager::decklink_supported()` = 构建开启 `AMV_ENABLE_DECKLINK_OUTPUT` **且** `obs_get_output_flags("decklink_output") != 0`（OBS Windows 标准分发自带 decklink 插件）。对话框据此灰置 DeckLink 页（沿用 NDI 灰置模式）。注意 `obs_output_create` 对未注册 id 返回**非 NULL 但惰性**对象，故以 flags 探测为准。

---

## 4. 文件改动（沿用 Spout/NDI 既有模式；§7 新 .cpp 必入 CMake）

**新增**
- `src/multiview-output-decklink.hpp` / `.cpp`：`DeckLinkOutputBackend` + 工厂 `create_decklink_output_backend()`。以 `multiview-output-ndi.cpp` 为模板。

**改动**
1. `src/multiview-instance.hpp`（`:679-705`）：`OutputBackendSettings` 加 `std::string deckDeviceHash; long long deckModeId=0; int deckKeyer=0; bool deckForceSdr=false;`（Spout/NDI 忽略；复用既有 `customWidth/customHeight` 存光栅、`audioMode/audioTrackIndex` 存音轨）。`InstanceOutputSettings` 加 `OutputBackendSettings decklink;`，`any_enabled()` 并入 `|| decklink.enabled`。
2. `src/multiview-instance-serialize-output.cpp`：`OutputBackendSettings::to/from_obs_data`（`:119-173`）序列化 4 新字段，`obs_data_has_user_value` 守卫（向后兼容，无需 bump configVersion）；`InstanceOutputSettings::to/from_obs_data`（`:175-199`）加 `"decklink"` 子对象键。
3. `src/multiview-output.hpp`（`:122-146`）：`Kind{Spout,Ndi,Decklink}`；加成员 `BackendEntry decklink_;`；声明 `static bool decklink_supported();`。
4. `src/multiview-output.cpp`：`#ifdef AMV_ENABLE_DECKLINK_OUTPUT #include …#endif`；`decklink_supported()`；`backend_available`(`:57`)/`create_backend`(`:68`)/`reconcile` 日志串(`:82-108`) 加 Decklink 分支；`render_all`(`:163`) 加 `reconcile(decklink_, cfg.decklink, Kind::Decklink)`；**三处 `BackendEntry *entries[]`（`:155`、`:177`、`:199`）全部加 `&decklink_`**（漏一处即静默丢该后端——易错点）；`teardown_locked`(`:231`)/`shutdown_graphics` 空判(`:251`) 并入。
5. `src/external-output-settings-dialog.hpp`/`.cpp`：`BackendWidgets`(hpp:34) 加可空 HW 控件 `QComboBox *deckDevice/deckMode/deckKeyer; QCheckBox *deckForceSdr;`；加成员 `decklink_`。`setup_ui` 增 DeckLink 页（`available=decklink_supported()`，不可用整页灰置+原因）；DeckLink 页**不含** resMode/custom/fps 控件，含 enabled+设备+模式+keyer+forceSdr+音频（audioMode+audioTrack，supportsAudio=true）。设备/模式枚举见 §5。`load_backend`/`read_backend`(`:51-52`) 处理新字段。
6. `CMakeLists.txt`（`:112-148` 区，紧邻 SPOUT/NDI 门控）：`option(ENABLE_DECKLINK_OUTPUT "…" ON)` → `target_compile_definitions(… AMV_ENABLE_DECKLINK_OUTPUT)`，新 .cpp 入源列表（guard 下）。**无 SDK 查找/链接**。
7. `data/locale/en-US.ini` + `zh-CN.ini`：`AMVPlugin.Output.Tab.DeckLink` 及设备/模式/keyer/ForceSDR 标签、FPS 不匹配提示键。**两文件 key 集合必须一致**（deploy 脚本会拦）。

---

## 5. 设备/模式枚举（对话框，无 SDK）
`obs_get_output_properties("decklink_output")` → 取 `device_hash` 列表项（`obs_property_list_item_count/string`）填设备下拉（data=hash）。设备变更：把 `device_hash` 写入临时 `obs_data`，`obs_property_modified(deviceProp, settings)` 触发模块 `decklink_output_device_changed` 回调，它按当前画布 fps 过滤填充 `mode_id` 列表；再遍历填模式下拉（`obs_property_list_item_int`，data=id）。选定模式→临时 create 读 `obs_output_get_video_conversion` 得光栅写入 custom 宽高（§3.4）。keyer 选项 Disabled/External/Internal（0/1/2）。

---

## 6. 取舍（considered & 选择理由）
- **复用 `decklink_output` vs 自接 DeckLink SDK**：选复用。零 SDK 依赖、零设备枚举/调度/像素打包代码、随 OBS 升级自动获益；代价是受其约束（FPS 严格相等、音频强制）。
- **UI 线程生命周期 vs 图形线程同步**：选 UI 线程。图形线程同步 release 会 hitch 主节目输出（§2 约束 2），虽实现更简单但违反 §0——拒绝。
- **共享 struct 加字段 vs 专用 struct**：选给 `OutputBackendSettings` 加 4 个 DeckLink 字段。复用既有 serialize / `configure_audio(cfg)` 传参通道、改动最小；Spout/NDI 无害忽略。
- **锁定模式光栅 vs 交 OBS 缩放**：选锁定（用户决策）。零缩放、画质最稳。

---

## 7. 验证（§3 流程：clang-format → build Debug+Rel → deploy → 用户实测 → 提交）
1. （可选）Phase 0 探针：运行时确认 `obs_get_output_flags("decklink_output")!=0`、枚举设备、读某模式 `obs_output_get_video_conversion`。不提交。
2. 构建：clang-format 19.1.1 + gersemi 检查 → `cmake --build build_x64 --config Debug` 与 `RelWithDebInfo` → `.\docs\setup\deploy-plugin.ps1 RelWithDebInfo`。
3. **有 DeckLink 卡**：对话框启用→选设备+模式（已按画布 fps 过滤）→确认 SDI/HDMI 正确出多画面；切 keyer/forceSdr；切音频模式/音轨；反复启用/禁用/关窗/切场景集合 → OBS 绝不崩、绝不卡（重点看主节目输出无 hitch）。
4. **无卡兜底**：设备下拉空 / 页灰置，启用为安全 no-op，无崩溃。
5. 并发压测（发版前硬化，§发版）：反复启停 + 改画布 fps 使模式失效 + 与 NDI/Spout 同时启用。

---

## 8. 分阶段
- **P1** 后端 + manager 接线 + 配置/序列化（临时/最小 UI 路径即可先出画）：SDI 出画、启用/禁用稳定、主节目无 hitch。
- **P2** 完整对话框：设备/模式/keyer/forceSdr/音频下拉、枚举、光栅锁定、fps 过滤、配置往返持久化。
- **P3 硬化**：UI-线程生命周期与 video_ 跨线程竞态审计、画布 fps 变更致模式失效的校验+提示、尺寸变更重开、None 静音路径实测、DeckLink+NDI+Spout 并存、反复启停压测；沉淀 `docs/issue-16-decklink-output-hardening-notes.md`。可选：把 `entries[]` 重构成单容器消除「漏改数组」隐患。
- **后续（AJA）**：机制同 DeckLink（`aja_output`, `OBS_OUTPUT_AV`），但需 NTV2 SDK 构建的 OBS 且插 AJA 硬件才注册；`aja_supported()` 用 `obs_get_output_flags("aja_output")!=0` 运行时探测、不可用灰置。无 AJA 硬件无法真机验证发送链路，故独立成阶段。

---

## 9. 崩溃面 / 锁序 / 线程 清单（审阅对照）
- **崩溃面**：video_t 被 output 捕获线程与图形线程双读；销毁顺序错 = UAF。→ §3.2 拆解三步序固定。
- **锁序**：本后端自有 `mtx_` 只守 `video_` 指针，**不嵌套任何 OBS 锁**；图形线程持 `mtx_` 期间不调 `obs_enter_graphics`（本就在图形上下文内）、不调会 join 的 output API（那些在 UI 任务里、锁外）。无新嵌套锁序。
- **线程**：重活（create/release/close/硬件）全在 UI 线程任务；图形线程只快操作。`obs_frontend_*` 只经主线程缓存读，绝不在图形线程调。
- **幂等**：stop() 未启动也安全；冷却防启动失败刷屏；pending 标志防重复投递 UI 任务。
