# Signal-Lost v2 — migrating the render path to the unified model (deferred)

> Status: **design only — not scheduled.** Captured so the option is
> well-understood when/if we decide the gained capabilities are worth the risk.
> Related: `docs/signal-lost-redesign-design.md`, issue #5.

## 1. Where we are today (the bridge)

Signal-Lost v2 (stages 1 / 2a / 2b / C) made the **dialog + serialization**
speak a unified model:

- `LostDisplayContent { Black, LastFrame, Fallback, ClearCell }` (axis B1)
- `LostStatusBand { None, SignalLost, Reconnecting, Auto }` (axis B2)
- `RecoveryPolicy { Auto, ManualOnly }` (axis A)
- one merged fallback image path (`fallbackName` when `fallbackType == "image"`)

But the **hardened render path was deliberately left untouched.** It still
reads the legacy fields:

- `internalMissingBehavior`, `externalLostBehavior`
- `placeholderImagePath` / `signalLostImagePath` (+ their fit modes)
- `fallbackType` / `fallbackName` / `fallbackImageFitMode`

Two functions bridge the two worlds (`multiview-instance-serialize-signal.cpp`):

- `derive_legacy_lost_fields()` — projects the v2 fields back onto the legacy
  fields after every load and after the dialog collects settings. The render
  consumes the derived legacy fields.
- `migrate_lost_settings_v1_to_v2()` — lifts a pre-v2 config up into the v2
  fields (external behavior wins when it is non-default).

The one exception already migrated to read v2 directly is the **status band**:
`render_status_overlay()` reads `cs.effective_lost.statusBand` so `None` truly
suppresses the band and `SignalLost`/`Reconnecting` force the colour.

## 2. What switching the render to the v2 model would INTRODUCE

1. **A real "force black" vs "keep last frame" distinction for external cells.**
   Today both `Black` and `LastFrame` map to `SignalLostOverlay`, which keeps
   the private source's last/black frame on screen — they are indistinguishable.
   A native v2 render path could:
   - `Black` → paint the cell black (ignore the frozen source frame);
   - `LastFrame` → explicitly keep the last decoded frame.
2. **Cleaner, single-path render logic.** The internal/external branching in
   `draw_cells()` + `compute_wanted_lost_image_path()` collapses into one
   `displayContent`-driven path.
3. **statusBand fully orthogonal everywhere** (already true for the overlay;
   the rest of the render would stop depending on the externalLostBehavior
   colour side effects).

## 3. What it would REMOVE

- The legacy fields from `LostSignalSettings`: `internalMissingBehavior`,
  `externalLostBehavior`, `placeholderImagePath`, `signalLostImagePath`, and
  their fit modes — deleted from the struct.
- `derive_legacy_lost_fields()` as a **runtime** dependency (it would only be
  needed, if at all, for one more migration cycle).
- The internal/external dual branches in `draw_cells` / `lost-image`.
- The documented approximations: the "internal LastFrame degrades to Black"
  note, and the Black-vs-LastFrame ambiguity for external cells.

## 4. The cost / risk (why it is deferred)

The code that must change is the **issue #5 crash-safety-critical render
path** — the exact logic that took stages A/B/hardening to make safe against
the streamdeck-plugin-obs `signal_handler_signal+0x122` crash:

- `amv-instance-core-draw.cpp` — state classification, the fallback drop-src
  block, the `fallback_latched` sticky logic, the tracked-fallback-slot
  resolution, all keyed on the legacy enums.
- `amv-instance-core-lost-image.cpp` — `compute_wanted_lost_image_path()` is
  keyed on `internalMissingBehavior` / `externalLostBehavior` / `fallbackType`.
- `amv-instance-core-status.cpp` — kind selection (partly migrated).

Rewriting these risks regressing the carefully-built fallback / overlay /
crash-avoidance behavior. The **functional gain is modest** (force-black for
external + a frame-vs-black distinction) relative to that regression risk, and
everything works today through the bridge.

## 5. If we do it — suggested approach

1. Add `displayContent` consumption to `draw_cells` behind the existing tracked
   fallback slot (reuse, do not rewrite, the slot lifecycle from stage B).
2. Port `compute_wanted_lost_image_path()` to `displayContent` (the image path
   is already the single merged `fallbackName`/`fallbackImageFitMode`).
3. Delete the legacy fields + `derive_legacy_lost_fields` once no consumer
   reads them.
4. Re-run the full streamdeck stress matrix (issue #5 §11 verification) — this
   is mandatory, not optional, because the change touches the crash path.
5. Keep `migrate_lost_settings_v1_to_v2` for one release so old configs still
   load.

## 6. Recommendation

**Do not schedule** unless the "force black for external" or explicit
"keep last frame" capabilities are specifically requested. The bridge is
complete, correct, and keeps the hardened render path untouched.
