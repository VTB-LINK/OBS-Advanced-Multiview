# OBS Isolation / Broadcast-Grade Review (pre-Phase-2, issue #10)

> **Goal (overarching, not just #10):** the plugin must never crash, deadlock,
> block, or stall OBS. A failure on an OBS thread takes down the whole broadcast.
> Whether we READ from OBS or HAND data back, we must not interfere.
>
> This review covers the plan + post-phase-3 code (incl. issue #11 output and the
> issue #10 Phase-1 core/view split). It is **verified again after Phase 2, before
> hardening**. Findings below are adjudicated against the actual OBS source at
> `C:\Users\oldking139\Documents\Repos\Github\obs-studio` — several raw audit
> findings were over-stated and are corrected here.

## Threads in play
- **Graphics thread** (single, serialized): every visible view's display draw
  callback → `MultiviewWindow::render` → `AmvInstanceCore::draw_cells`; the
  per-frame `tick_once_per_frame`; the global output driver `on_main_rendered` →
  `render_output_only` (+ Spout/NDI submit). **Stalling this = dropped broadcast frames.**
- **Audio thread**: NDI `on_audio` capture callback; `volmeter_callback`.
- **OBS signal threads**: `source_create/remove/destroy/rename`, `mute`,
  `audio_mixers` — fire on whatever thread raised them.
- **UI/main thread**: config edits (`refresh_*`), dialogs, `config_->save()`.

---

## Findings (adjudicated severity)

### F1 — NDI GPU readback stalls the graphics thread — **REAL, MED-HIGH (output on)**
`multiview-output-ndi.cpp` `submit_frame`: `gs_stage_texture` then **`gs_stagesurface_map` blocks** the graphics thread until the GPU copy lands (single staging surface, no double-buffer). Only when NDI output is enabled, once per output frame. On the graphics thread → can delay the program composite → dropped frames.
**Fix (hardening):** double-buffer — `gs_stage_texture` frame N, `gs_stagesurface_map` frame N-1 (one frame of latency, no sync stall). Already noted in issue-11 hardening notes. *Spout shares the texture on-GPU (no readback) → not affected.*

### F2 — Render-thread `obs_frontend_*` races a main-thread scene switch — **REAL, MED (low-prob, high-impact)**
`obs_frontend_get_current_scene/preview_scene/streaming_output` are called from the graphics thread (`draw_cells` PGM/PRVW resolve, `tick`→highlight trees, `compute_active_track_bit`). Verified in OBS source: `OBSStudioAPI::obs_frontend_get_current_scene()` reads `OBSBasic::programScene` (an `OBSWeakSource` member) / `GetCurrentSceneSource()` **with no lock**. A render-thread read racing a main-thread scene change is a torn read of a ref-counted handle → low-probability use-after-free → OBS crash.
**Note:** Phase 2 (N views) MULTIPLIES the per-cell PGM/PRVW frontend calls (once per view per frame). So this gets worse with Phase 2.
**Fix:** cache the current/preview scene (and streaming-track) refs via frontend **event callbacks** that fire on the main thread (`OBS_FRONTEND_EVENT_SCENE_CHANGED` / `PREVIEW_SCENE_CHANGED` / `STUDIO_MODE_*` / streaming start/stop), store under a tiny mutex (or atomically), and have the render thread read the cache — **zero `obs_frontend_*` calls on the render thread**. This both removes the race and removes the per-view multiplication. *Recommended to land before/with Phase 2 so Phase 2 inherits the safe pattern.*

### F3 — `source_remove` handler taking `source_mutex_` synchronously — **adjudicated LOW (agent said HIGH)**
Verified: `obs_source_remove` (libobs/obs-source.c:927) fires the `remove` signal via `obs_source_dosignal` **without holding any libobs source lock**, on the (main) thread that removed it. So `on_source_being_removed` taking `source_mutex_` synchronously cannot ABBA-deadlock the render thread (which holds `source_mutex_` + OBS scene locks) — there is no shared OBS lock in the cycle; it's plain mutual exclusion (brief wait). The synchronous path is **intentional and correct** (closes the window before OBS prunes sceneitems, which has crashed other plugins). **Do NOT defer it** — deferring would widen the race for no safety gain. ✓ Keep as-is.

### F4 — `source_mutex_` held across `obs_source_video_render` — **adjudicated LOW (agent said HIGH/unbounded)**
`draw_cells` holds `source_mutex_` during `obs_source_video_render`. Its cost is the *normal* OBS source render cost (same as the program render) — our lock doesn't extend it. The only contention is a UI-thread `refresh_*` waiting briefly; and `refresh_*` does its expensive source create/destroy **outside** `source_mutex_` (4-phase pattern), so it never holds the lock long enough to stall a render frame. The streamdeck-prune crash is guarded by the `obs_source_removed()` re-check. ✓ Acceptable.

### F5 — `obs_enter_graphics` lock ordering — **LOW, verify+document**
Established invariant: render thread takes graphics-lock → `source_mutex_`; UI-thread teardown collects under `source_mutex_`, releases, **then** `obs_enter_graphics` to destroy textures (never nested) → no ABBA. `sender_mutex_` (NDI) is leaf (never taken with `source_mutex_`). **Action (hardening):** add an explicit lock-order note + a quick audit that no `obs_enter_graphics` is ever called while `source_mutex_` is held (esp. `apply_output_settings`, `rebuild_*image*`).

### Verified SAFE (no action)
- **inc_showing/dec_showing & inc_active/dec_active pairing** — traced across all paths (refresh_cell early-returns, layout shrink, clear-cell, core dtor); balanced. External cells use `inc_active` only (avoids double-count with the internal show ref). No leaks / double-drops.
- **Callback lifetime** — volmeter/mute/audio_mixers callbacks and NDI `on_audio` are disconnected before the object is freed; `release_volmeters` removes callback → detach → destroy in order; NDI `stop()` disconnects audio → (lock) destroy sender + drop runtime.
- **NDI audio threading** — `sender_mutex_` guards sender across audio/graphics; video send lock-free (graphics is sole sender-lifetime writer + NDI allows concurrent A/V send); `disconnect_audio` before teardown.
- **Signal handlers use atomics, not locks** (`mute`→atomic, `audio_mixers`→atomic flag, rebuild deferred to render thread + 250ms throttle).
- **obs_data_t / source ref leaks** — none found (all get/release paired).
- **config save** — UI-thread only; snapshot_cell holds a strong `private_source` ref so context-menu media controls can't race teardown.

---

## What Phase 2 (N views) must respect
1. **Do not add render-thread `obs_frontend_*` calls.** Prefer the F2 cached scene refs. N views already multiply existing ones — fixing F2 first is the clean path.
2. **`source_mutex_` / sources / volmeters / output stay single per core** (the whole point) — views never own stream-pulling state. (Phase-1 already enforces this.)
3. **`views_` is UI-thread-only**; the render driver list is rebuilt under `obs_enter_graphics`. Never let the graphics thread walk `views_`.
4. **Per-core tick once/frame** (token dedup) so N views don't N× the scene-tree walk / frontend access.
5. **Closing a view must not release shared sources** (only the last view + no output tears the core down) — preserves the inc/dec balance.

## Fix scheduling
- **Before/with Phase 2 (recommended):** F2 (frontend scene caching) — Phase 2 worsens it and it's the one real crash-race.
- **Hardening (post-Phase-2 re-verify):** F1 (NDI double-buffer readback), F5 (lock-order audit + doc). Re-run this whole review after Phase 2.

## Post-Phase-2 verification checklist (re-run before hardening)
- [ ] No `obs_frontend_*` on the graphics thread (grep `amv-instance-core*`), or all reads come from the main-thread-updated cache.
- [ ] Multiple views of one instance: one stream pull, one volmeter set, one tick/frame (no N× frontend/tree walk).
- [ ] Closing 1 of N views doesn't drop shared sources; closing last view (no output) balances all inc/dec.
- [ ] NDI output + multiple views: graphics-thread frame time stays within budget (readback not stalling); audio sender mutex never held during a blocking call.
- [ ] No `obs_enter_graphics` under `source_mutex_`; no new cross-thread `views_` access.
- [ ] Stress: rapid scene switching + source delete/undo while N views render + output on → no crash/deadlock.

---

## Post-Phase-2 re-verification result (after commit 9df8607)

Code-level (by inspection) — **PASS**:
- **[x] Item 1** — No `obs_frontend_*` on the graphics thread. Grep of `amv-instance-core*.cpp` finds only a *comment* in `-vu.cpp`; all scene/preview/streaming reads go through `amv_frontend::current_program_scene()/current_preview_scene()/streaming_mixers()` (main-thread-updated cache, F2 fix, commit 4aefa13). Whole-`src` grep confirms the only live `obs_frontend_*` reads are the cache itself + `multiview-window-context-menu.cpp` (UI handler) + `config-manager.cpp` (scene-collection name) — all main thread.
- **[x] Item 5** — Graphics-thread `on_main_rendered` iterates only `g_output_hosts` (rebuilt under `obs_enter_graphics` in `multiview_refresh_output_driver`); it never walks `g_views`/`g_cores`. The new `g_views`/`g_cores` registries are mutated UI-thread-only. No new `obs_enter_graphics` is taken under `source_mutex_` — the only new one (`multiview_refresh_output_driver`) holds no core lock.

Architectural (by inspection) — **PASS**:
- **[x] Item 2** — Exactly one `AmvInstanceCore` per uuid in `g_cores`; all N views hold a non-owning pointer to it, so sources/volmeters exist once. `tick_once_per_frame()` dedups via the per-core frame token, so N views + the output driver tick once/frame.
- **[x] Item 3** — `MultiviewWindow::closeEvent` tears down only the view's display; the core (and its `release_source_refs` inc/dec balance) is destroyed only when `on_window_closed` finds the last view gone AND no output (or `close_multiview_window`/unload). Closing 1 of N never touches the shared core.

UAF ordering (new in Phase 2) — **PASS by inspection**: every core teardown path
(`on_window_closed`, `close_multiview_window`, `reconcile_output_host`) moves the
core out of `g_cores` and calls `multiview_refresh_output_driver()` (rebuilds
`g_output_hosts` under `obs_enter_graphics`) **before** the core is destroyed, and
deletes views (removing their display callbacks) before the core dtor runs — so no
in-flight render frame can hold a freed core.

Runtime self-test (user) — pending, not code-checkable:
- **[ ] Item 2 (runtime)** — confirm a single m3u8 source opened in 2 windows makes ONE network connection (OBS log / resource monitor).
- **[ ] Item 4** — NDI output + 2 windows: graphics frame time within budget. (F1 single-buffer readback still present — deferred to hardening; Phase 2 added no regression: still one output pass per core.)
- **[ ] Item 6** — stress: rapid scene switch + source delete/undo while 2 windows render + output on → no crash/deadlock.

**Conclusion:** all code-checkable broadcast-grade items pass; no new render-thread
OBS calls, no new cross-thread registry access, UAF-safe teardown. Remaining open
items are the user runtime self-tests above and the two scheduled hardening fixes
(F1 readback double-buffer, F5 lock-order audit + doc).

---

## Hardening results (post-Phase-2)

### F1 — NDI readback double-buffer — TRIED, then REVERTED (e39bddd -> 02c4fb8)
Implemented a two-surface ping-pong (stage frame N, map+send frame N-1) to remove
the synchronous `gs_stagesurface_map` stall on the graphics thread. **Reverted
after runtime testing** because it traded a stall that wasn't actually dropping
frames on real hardware for two worse regressions for low-latency broadcast:
(1) +1 frame of video latency, clearly noticeable at 30fps (~33ms); (2) A/V
desync — only video was delayed a frame while audio (`on_audio`) sent
immediately, so with `timecode_synthesize` the receiver plays audio ~1 frame
ahead of video. **Decision: keep the synchronous single-buffer readback** — it's
low-latency and A/V-synced, the right trade here. The ~1-frame map stall remains
a documented theoretical risk for very slow GPUs only (not observed on an
RTX 3080). If ever revisited, a correct double-buffer MUST also delay audio by
one frame to preserve sync — but that re-adds the latency, so it's not worth it
for this use case.

### F5 — Lock-order audit + doc — DONE (audited, no code change needed)

**Locks in play** (innermost-last):
1. **OBS graphics lock** (`obs_enter_graphics`/`obs_leave_graphics`, and held by
   OBS around display draw callbacks + the main-rendered callback).
2. **`AmvInstanceCore::source_mutex_`** — recursive, one per core; guards all
   shared cell/source/volmeter/image state.
3. **Leaves (never held while acquiring 1 or 2):**
   - `NdiOutputBackend::sender_mutex_` — guards the NDI sender across the
     graphics (video send) and audio (capture) threads.
   - `amv_frontend` `g_mtx` — guards the program/preview/streaming cache.
   - `plugin-main` `g_cores` / `g_views` — **UI-thread-only**, no lock; the
     graphics thread reads only `g_output_hosts`, rebuilt under the graphics lock.

**The one legal nesting — render path:** OBS calls our display draw callback /
`on_main_rendered` **with the graphics lock held**; inside, `draw_cells` /
`render_output_only` take `source_mutex_`. So the only lock nesting is
**graphics → source_mutex_**.

**Teardown / rebuild never nests the other way.** Every site that needs the
graphics lock to destroy/create GPU objects (textures, vertex buffers, NDI stage
surfaces) follows the four-phase pattern: (1) collect handles under
`source_mutex_`, (2) **release** `source_mutex_`, (3) `obs_enter_graphics` to do
the GPU work, (4) re-lock to install. Verified: `obs_enter_graphics` appears only
**outside** any `source_mutex_` scope — `amv-instance-core-image.cpp:111/236`,
`-lost-image.cpp:209/258`, `-sources.cpp:1343/1378/1478` (release/rebuild),
`multiview-output.cpp:231`, `plugin-main.cpp:132` (no core lock there). The
expensive `obs_source_dec_active`/`dec_showing` (host-plugin worker teardown) and
`obs_source_create_private` likewise run only outside `source_mutex_`.

**Conclusion:** no path acquires the graphics lock while holding `source_mutex_`,
and no leaf lock is held while acquiring 1 or 2 → no ABBA cycle. The invariant is
"graphics → source_mutex_ only; release source_mutex_ before any
obs_enter_graphics." Keep new code on the four-phase pattern.

### Remaining hardening
- Split `multiview-instance.cpp` (2336 lines) — hygiene; fold in a teardown
  create/release audit for the small pre-existing shutdown leak (~8-14 OBS
  objects under heavy use; KB-scale, negligible runtime impact).
