/*
OBS Advanced Multiview
Copyright (C) 2025 VTB-LINK

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <obs-data.h>

/* ========== Visual Settings Enums ========== */

enum class InheritanceMode { Inherit, Override };

enum class LabelDisplayMode { None, Overlay, Below };

enum class LabelPosition { Top, Bottom };

enum class FontScaleMode { Fixed, ScaleWithCell };

enum class ImageFitMode { Fit, Stretch };

enum class SafeAreaPreset { EBU_R95 };

enum class VuMeterPosition { Left, Right, Bottom, Top };

enum class VuMeterStyle { Bar };

enum class VuMeterAnchorMode { Cell, Signal };

enum class VuMeterDecayRate { Fast, Medium, Slow };

enum class VuMeterAlignment { Start, Center };

/* Track selection mode for VU meter audio routing.
 *
 * v1 (Phase 2.5) values — kept for config backward compat:
 *   AutoFollowStreaming: follow OBS streaming output's mixer track (first set bit).
 *   Manual: user picks a fixed track (1..6) via manualTrackIndex.
 *
 * M6 additions (Phase 3 / step 8) — introduce provider-aware routing without
 * disturbing the internal-cell paths:
 *   Auto: for internal OBS cells (pgm/prvw/scene/source) behave exactly like
 *     AutoFollowStreaming; for external cells (ffmpeg/ndi/spout/vlc) meter
 *     the external private source directly (one meter, no scene walk).
 *   ExternalSource: explicit "prefer the cell's external private source for
 *     metering". For internal cells (no private source) falls back to the
 *     AutoFollowStreaming track-bit so the window does not silently lose
 *     metering on cells that don't have a provider yet.
 *
 * String tokens used by config persistence (see vu_meter_track_mode_to_str /
 * vu_meter_track_mode_from_str): "auto_follow_streaming", "manual", "auto",
 * "external_source". Unknown tokens fall back to AutoFollowStreaming. */
enum class VuMeterTrackMode { AutoFollowStreaming, Manual, Auto, ExternalSource };

enum class VuMeterScaleSide { Auto, Same, Opposite };

enum class OverlayFitMode { Fit, Stretch };

enum class OverlayAnchorMode { Cell, Signal };

enum class SafeAreaAnchorMode { Cell, Signal };

enum class BackgroundFillMode { FillSignalOnly, FillEntireCell };

/* ========== Visual Settings Group Structs ========== */

struct BackgroundSettings {
	bool colorEnabled = true;
	uint32_t color = 0xFF000000; /* ARGB black */
	BackgroundFillMode fillMode = BackgroundFillMode::FillEntireCell;
	bool labelRegionFill = false; /* Below mode: fill label row with bgColor */
	bool imageEnabled = false;
	std::string imagePath;
	ImageFitMode imageFitMode = ImageFitMode::Fit;

	obs_data_t *to_obs_data() const;
	static BackgroundSettings from_obs_data(obs_data_t *data);
};

struct LabelSettings {
	LabelDisplayMode displayMode = LabelDisplayMode::Overlay;
	LabelPosition position = LabelPosition::Bottom;
	std::string fontFamily; /* empty = system default */
	int fontSize = 14;
	FontScaleMode fontScaleMode = FontScaleMode::ScaleWithCell;
	int minFontSize = 8;
	int maxFontSize = 80;
	uint32_t textColor = 0xFFFFFFFF; /* ARGB white */
	double backgroundOpacity = 0.2;
	bool backgroundRounded = false;
	int margin = 4;

	obs_data_t *to_obs_data() const;
	static LabelSettings from_obs_data(obs_data_t *data);
};

struct SafeAreaSettings {
	bool enabled = false;
	SafeAreaPreset preset = SafeAreaPreset::EBU_R95;
	SafeAreaAnchorMode anchorMode = SafeAreaAnchorMode::Cell;
	uint32_t color = 0xFFD0D0D0; /* ARGB light grey - matches OBS native OUTLINE_COLOR */
	double opacity = 1.0;

	obs_data_t *to_obs_data() const;
	static SafeAreaSettings from_obs_data(obs_data_t *data);
};

struct VuMeterSettings {
	bool enabled = true;
	VuMeterPosition position = VuMeterPosition::Left;
	double opacity = 0.75;
	int width = 24;
	VuMeterStyle style = VuMeterStyle::Bar;
	VuMeterAnchorMode anchor = VuMeterAnchorMode::Cell;
	bool flip = false;
	double lengthRatio = 1.0; /* 0.0~1.0, fraction of edge length */
	double warningDB = -20.0; /* green->yellow threshold */
	double errorDB = -9.0;    /* yellow->red threshold */
	VuMeterDecayRate decayRate = VuMeterDecayRate::Fast;
	VuMeterAlignment alignment = VuMeterAlignment::Center;

	/* Track routing — controls which mixer track determines source visibility.
	 * AutoFollowStreaming: follow streaming output's first mixer bit (default,
	 * matches "what audience hears" semantics).
	 * Manual: pin to manualTrackIndex (1..6).
	 *
	 * Sources whose audio_mixers bitmask does NOT intersect the active track
	 * are auto-excluded from the VU meter (they contribute zero audio to PGM). */
	VuMeterTrackMode trackMode = VuMeterTrackMode::AutoFollowStreaming;
	int manualTrackIndex = 1; /* 1..6, only used when trackMode == Manual */

	/* Peak Hold — Phase 2.5 polish */
	bool peakHoldEnabled = true;
	int peakHoldMs = 1500;                /* 100 ~ 5000 */
	double peakHoldDecayDbPerSec = 11.76; /* 1.0 ~ 60.0, matches Medium decay */
	int peakHoldWidthPx = 3;              /* 1 ~ 4 */

	/* dB Scale / Ticks — Phase 2.5 polish */
	bool scaleEnabled = true;
	std::string scaleTicks; /* CSV dB values; empty = default "-60,-40,-20,-9,0" */
	bool scaleShowLabels = true;
	uint32_t scaleColor = 0x80FFFFFF; /* ARGB half-transparent white */
	VuMeterScaleSide scaleSide = VuMeterScaleSide::Same;

	obs_data_t *to_obs_data() const;
	static VuMeterSettings from_obs_data(obs_data_t *data);
};

struct OverlaySettings {
	bool enabled = false;
	std::string imagePath;
	double opacity = 1.0;
	OverlayFitMode fitMode = OverlayFitMode::Fit;
	OverlayAnchorMode anchorMode = OverlayAnchorMode::Cell;

	obs_data_t *to_obs_data() const;
	static OverlaySettings from_obs_data(obs_data_t *data);
};

/* ========== Signal Lost / Missing Behavior (Phase 3 / M5) ==========
 *
 * Behavior selected when a cell's primary signal is unavailable. Two
 * orthogonal sources of unavailability:
 *
 *   - Internal source missing: assigned OBS scene/source no longer
 *     resolvable by name (deleted, scene collection switch, etc.).
 *   - External source lost: provider-backed private source has no recent
 *     valid output (reserved for M6; data model already future-proof).
 *
 * Defaults follow [docs/phase-3-signal-lost-and-external-sources-design.md]
 * §5: black + 'Missing Source' overlay for internal, Signal Lost overlay for
 * external; fallback off; manual reconnect cooldown 1000 ms.
 */
enum class InternalMissingBehavior {
	Black,            /* keep cell black + 'Missing Source' overlay (default) */
	PlaceholderImage, /* show user-selected static image + overlay */
	ClearCell,        /* clear assignment and release cell (must be explicit) */
};

enum class ExternalLostBehavior {
	SignalLostOverlay, /* keep provider source alive, draw 'Signal Lost' overlay (default) */
	RetryOnly,         /* same as overlay, but fallback never engages */
	RetryWithFallback, /* show fallbackAssignment while monitoring main signal */
	SignalLostImage,   /* show signalLostImagePath when lost */
};

struct LostSignalSettings {
	InternalMissingBehavior internalMissingBehavior = InternalMissingBehavior::Black;
	ExternalLostBehavior externalLostBehavior = ExternalLostBehavior::SignalLostOverlay;

	/* Optional resource paths */
	std::string placeholderImagePath; /* used when internal == PlaceholderImage */
	std::string signalLostImagePath;  /* used when external == SignalLostImage */

	/* Phase 3 / M5.4: how the placeholder / signal-lost / fallback static
	 * image is fitted into the cell. Reuses the same enum as background
	 * images so users get consistent semantics across the plugin. Default
	 * is Stretch — placeholder + fallback images are usually authored at
	 * the cell's intended aspect ratio (or a generic 16:9 banner) and the
	 * user almost always wants them to fill the cell. Fit is preserved as
	 * an opt-in for users who want the original aspect kept (with bars). */
	ImageFitMode placeholderImageFitMode = ImageFitMode::Stretch;
	ImageFitMode signalLostImageFitMode = ImageFitMode::Stretch;
	ImageFitMode fallbackImageFitMode = ImageFitMode::Stretch;

	/* Fallback signal — Phase 3 first-pass supports static image and OBS
	 * internal sources only (mirror of CellAssignment). External fallback
	 * is reserved for later. fallbackType empty == disabled. */
	std::string fallbackType; /* "", "image", "pgm", "prvw", "scene", "source" */
	std::string fallbackName; /* image path (when type == image) or source name */

	/* Backoff timing for monitor-layer reconnect attempts. Provider plugins
	 * have their own internal retry loops; these only gate Multiview's
	 * private-source rebuild attempts. */
	int retryInitialMs = 1000;
	int retryMaxMs = 30000;
	int manualReconnectCooldownMs = 1000;

	obs_data_t *to_obs_data() const;
	static LostSignalSettings from_obs_data(obs_data_t *data);
};

/* PGM / PRVW cell highlight border (OBS native-style red/green borders).
 *
 * Scope is intentionally window-wide (Global + Instance only): per-cell
 * override is meaningless because the highlight is driven entirely by the
 * cell's *relationship* to the active PGM/PRVW scene tree. Per-cell tab in
 * the dialog disables this group. Cells nested inside PGM/PRVW (e.g. cell
 * shows scene X which is contained in current PGM) draw a dashed border in
 * the same color as the direct match — `nestedDashed` toggles the dashing. */
struct HighlightSettings {
	bool enabled = true;
	uint32_t pgmColor = 0xFFD00000;  /* ARGB red — matches OBS native */
	uint32_t prvwColor = 0xFF00D000; /* ARGB green — matches OBS native */
	bool nestedDashed = true;        /* nested matches use dashed border */
	int dashLengthPx = 12;           /* [4, 32] */
	int dashGapPx = 6;               /* [2, 16] */
	int minThicknessPx = 8;          /* fallback when gutter == 0; [2, 16] */

	obs_data_t *to_obs_data() const;
	static HighlightSettings from_obs_data(obs_data_t *data);
};

/* ========== Visual Settings Containers ========== */

struct GlobalVisualSettings {
	BackgroundSettings background;
	LabelSettings label;
	SafeAreaSettings safeArea;
	VuMeterSettings vuMeter;
	OverlaySettings overlay;
	HighlightSettings highlight;

	obs_data_t *to_obs_data() const;
	static GlobalVisualSettings from_obs_data(obs_data_t *data);
};

struct InstanceVisualSettings {
	InheritanceMode backgroundMode = InheritanceMode::Inherit;
	BackgroundSettings background;
	InheritanceMode labelMode = InheritanceMode::Inherit;
	LabelSettings label;
	InheritanceMode safeAreaMode = InheritanceMode::Inherit;
	SafeAreaSettings safeArea;
	InheritanceMode vuMeterMode = InheritanceMode::Inherit;
	VuMeterSettings vuMeter;
	InheritanceMode overlayMode = InheritanceMode::Inherit;
	OverlaySettings overlay;
	InheritanceMode highlightMode = InheritanceMode::Inherit;
	HighlightSettings highlight;

	obs_data_t *to_obs_data() const;
	static InstanceVisualSettings from_obs_data(obs_data_t *data);
};

struct CellVisualSettings {
	int row = -1;
	int col = -1;
	InheritanceMode backgroundMode = InheritanceMode::Inherit;
	BackgroundSettings background;
	InheritanceMode labelMode = InheritanceMode::Inherit;
	LabelSettings label;
	InheritanceMode safeAreaMode = InheritanceMode::Inherit;
	SafeAreaSettings safeArea;
	InheritanceMode vuMeterMode = InheritanceMode::Inherit;
	VuMeterSettings vuMeter;
	InheritanceMode overlayMode = InheritanceMode::Inherit;
	OverlaySettings overlay;

	obs_data_t *to_obs_data() const;
	static CellVisualSettings from_obs_data(obs_data_t *data);
};

struct EffectiveCellVisualSettings {
	BackgroundSettings background;
	LabelSettings label;
	SafeAreaSettings safeArea;
	VuMeterSettings vuMeter;
	OverlaySettings overlay;
	HighlightSettings highlight; /* always resolved from instance/global (no per-cell) */
};

/* Resolve effective visual settings via group-level inheritance chain:
 * cell (if override) -> instance (if override) -> global */
EffectiveCellVisualSettings resolve_effective_visual_settings(const GlobalVisualSettings &global,
							      const InstanceVisualSettings &instance,
							      const CellVisualSettings *cell);

/* ========== Core Data Structs ========== */

/* Phase 3 / M6: signal provider type. The persisted string form is the
 * single authoritative identifier; never serialize the enum integer. The
 * internal_* values mirror the M5 `CellAssignment.type` strings exactly so
 * legacy configs continue to load without an explicit migration step.
 *
 * `Unknown` is reserved for forward-compat: a future provider id stored on
 * disk that this build does not understand resolves to `Unknown`, the cell
 * stays empty, and the original `signalConfig` payload (kept verbatim) is
 * preserved on save so the user can keep using a newer build later.
 */
enum class SignalProviderType {
	Unknown,
	InternalPgm,
	InternalPrvw,
	InternalScene,
	InternalSource,
	Ffmpeg,
	Ndi,
	Spout,
	Vlc,
	WebRtcReserved,
};

const char *signal_provider_to_string(SignalProviderType p);
SignalProviderType signal_provider_from_string(const char *s);

/* True for `internal_*` providers whose runtime is the existing M5 path
 * (`CellAssignment.type` + `name` + OBS scene/source weak ref). External
 * providers own a private OBS source and a richer settings object. */
bool signal_provider_is_internal(SignalProviderType p);

/* Platform support for an external provider.
 *
 * Returns false when the provider is fundamentally impossible on the
 * current OS (e.g. Spout depends on Windows DirectX shared textures,
 * obs-spout2 ships no macOS / Linux build). Returns true for internal
 * providers, for cross-platform external providers (FFmpeg, NDI, VLC),
 * and for the reserved WebRTC slot.
 *
 * This is a static, compile-time-resolved check separate from
 * ISignalProvider::is_available() — it answers "could this OBS install
 * ever host the provider?" rather than "is the host plugin loaded right
 * now?". UI surfaces use the platform check to give an accurate reason
 * ("Windows-only" vs "plugin not installed") and to disable Save in
 * EditSourceDialog when a config imported from another OS targets a
 * provider this OS cannot run. */
bool signal_provider_supported_on_platform(SignalProviderType p);

/* Stable, human-readable reason returned when
 * signal_provider_supported_on_platform() is false. Empty string when
 * the provider is supported on the current platform. */
const char *signal_provider_unsupported_platform_reason(SignalProviderType p);

/* Phase 3 / M6: external provider configuration payload.
 *
 * `provider == Unknown` means "no external config / use legacy CellAssignment
 * type+name semantics". For external providers, `providerSettings` holds the
 * provider-specific keys (e.g. `ndi_source_name`, `spout_capture` settings,
 * FFmpeg `input` URL); the registry decides which keys to read.
 *
 * Round-trip rules (see CellAssignment::to/from_obs_data for behavior):
 *  - Internal cells never write `signalConfig` to disk; round-trips stay
 *    byte-compatible with M5 v3 configs.
 *  - External cells write `signalConfig` and clear the legacy `type/name`
 *    string-id semantics; render code reads provider runtime instead.
 *  - Unknown providers (forward-compat) preserve the raw OBS data so a
 *    future build can read it back losslessly.
 */
struct SignalConfig {
	SignalProviderType provider = SignalProviderType::Unknown;
	std::string displayName;                /* user-visible label for SourcePicker / VU label fallback */
	obs_data_t *providerSettings = nullptr; /* owned; may be nullptr when no settings yet */

	SignalConfig() = default;
	SignalConfig(const SignalConfig &other);
	SignalConfig(SignalConfig &&other) noexcept;
	SignalConfig &operator=(const SignalConfig &other);
	SignalConfig &operator=(SignalConfig &&other) noexcept;
	~SignalConfig();

	bool empty() const { return provider == SignalProviderType::Unknown && !providerSettings; }
	bool is_internal() const { return signal_provider_is_internal(provider); }
	bool is_external() const { return !empty() && !is_internal(); }

	obs_data_t *to_obs_data() const;
	static SignalConfig from_obs_data(obs_data_t *data);
};

struct CellAssignment {
	int row = -1;     // grid row position (-1 = legacy/unset)
	int col = -1;     // grid col position (-1 = legacy/unset)
	std::string type; // "pgm", "prvw", "scene", "source", ""
	std::string name; // scene/source name, empty for pgm/prvw/empty

	/* Phase 3 / M6: optional external provider config.
	 *
	 * For internal cells (pgm/prvw/scene/source/empty) this stays empty
	 * and is NOT serialized; M5 v3 configs round-trip unchanged.
	 *
	 * For external cells (ffmpeg/ndi/spout/vlc/webrtc_reserved) the
	 * provider-specific settings live here; the legacy `type` is set to
	 * a stable provider tag (`external`) so older code paths that filter
	 * by `type.empty()` still know the cell is occupied. */
	SignalConfig signalConfig;

	obs_data_t *to_obs_data() const;
	static CellAssignment from_obs_data(obs_data_t *data);
};

struct SpanRegion {
	int row;
	int col;
	int rowSpan;
	int colSpan;

	obs_data_t *to_obs_data() const;
	static SpanRegion from_obs_data(obs_data_t *data);
};

struct LayoutData {
	int rows = 4;
	int columns = 4;
	int gutterPx = 4;
	std::vector<SpanRegion> spans;

	obs_data_t *to_obs_data() const;
	static LayoutData from_obs_data(obs_data_t *data);
};

/* Per-cell Lost Signal override.
 *
 * Phase 3 uses a 2-layer model (Global + Cell) — instance-level override
 * is intentionally NOT introduced in M5 to keep the UI surface and JSON
 * shape minimal. If future feedback warrants it, an `InstanceLostSignalSettings`
 * container can be inserted between Global and Cell without breaking this
 * struct's persistence (fields are forward-compatible).
 *
 * `mode == Inherit` ⇒ resolver returns the global default and ignores the
 * `settings` payload. We still persist the payload so users can toggle
 * inheritance on/off without losing their previous overrides.
 */
struct CellLostSignalSettings {
	int row = -1;
	int col = -1;
	InheritanceMode mode = InheritanceMode::Inherit;
	LostSignalSettings settings;

	obs_data_t *to_obs_data() const;
	static CellLostSignalSettings from_obs_data(obs_data_t *data);
};

/* Resolve effective Lost Signal settings for a given (row, col).
 * cell == nullptr || cell->mode == Inherit ⇒ returns the global default. */
LostSignalSettings resolve_effective_lost_signal(const LostSignalSettings &global, const CellLostSignalSettings *cell);

struct MultiviewInstance {
	std::string uuid;
	std::string name;
	std::string folder; /* UI-only grouping tag, empty = root */
	LayoutData layout;
	std::vector<CellAssignment> cellAssignments;
	InstanceVisualSettings visualSettings;
	std::vector<CellVisualSettings> cellVisualSettings;

	/* Phase 3 / M5: per-cell Lost Signal overrides. Empty = all cells inherit
	 * the global default. Persisted only for cells with mode == Override. */
	std::vector<CellLostSignalSettings> cellLostSignalSettings;

	bool useGlobalGutter = true;
	bool layoutDirty = false;
	bool signalDirty = false;

	int effective_gutter(int globalGutter) const;

	/* Find cell visual settings for given coordinate, or nullptr */
	const CellVisualSettings *find_cell_visual(int row, int col) const;

	/* Find cell Lost Signal override for given coordinate, or nullptr */
	const CellLostSignalSettings *find_cell_lost_signal(int row, int col) const;

	obs_data_t *to_obs_data() const;
	static MultiviewInstance from_obs_data(obs_data_t *data);

	static MultiviewInstance create_new(const std::string &name);
	MultiviewInstance clone_instance(const std::string &newName) const;
};

struct LayoutPreset {
	std::string uuid;
	std::string name;
	LayoutData layout;

	obs_data_t *to_obs_data() const;
	static LayoutPreset from_obs_data(obs_data_t *data);
};

struct GlobalSettings {
	int defaultGutterPx = 7;
	bool reResolveInheritObs = true;
	double reResolveCustomFps = 30.0;
	GlobalVisualSettings visualSettings;

	/* Phase 3 / M5: project-wide default Lost Signal behavior. Cells without
	 * an Override entry resolve to this struct. */
	LostSignalSettings lostSignal;

	/* Phase 3 hardening tail: "Detailed logs" toggle. When false (default),
	 * high-frequency diagnostic INFO logs ([perf] every 5s, [health] retry
	 * chatter, [fill] aspect/snap, VU rebuild summaries, provider "created
	 * private source" success lines) are suppressed. WARNING / ERROR are
	 * always emitted regardless. The runtime mirror of this flag lives in
	 * amv-logging.cpp as a process-wide atomic; ConfigManager pushes the
	 * value there on load and the Manager Settings tab pushes it on Apply
	 * so static-context provider code can read it cheaply. */
	bool detailedLogs = false;

	obs_data_t *to_obs_data() const;
	static GlobalSettings from_obs_data(obs_data_t *data);
};
