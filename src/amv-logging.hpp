/*
OBS Advanced Multiview - Project logging helpers (Phase 3 / M6.6 hardening tail).

Thin C++ layer above the existing C ABI `obs_log()` wrapper in
`plugin-support.c.in`. Goals:

  - Centralise the "Detailed logs" runtime flag so business code does
    not have to thread a config pointer through every interface; in
    particular signal-provider-*.cpp files run in static contexts that
    cannot reach MultiviewWindow / ConfigManager directly.
  - Keep ERROR / WARNING always-on; only diagnostic INFO chatter is
    gated behind the flag.
  - Keep the underlying sink (OBS blogva via obs_log) unchanged so
    output formatting and the `[obs-advanced-multiview]` plugin prefix
    stay identical.
  - Stay lightweight: free functions + a single std::atomic<bool>;
    no Qt, no singletons, no thread ownership, no log files.

The migration policy for existing call sites is gradual. A site stays
on `obs_log()` until it actually proves noisy or until it would benefit
from the detailed gating. Not every line in the codebase needs to move.
*/

#pragma once

#ifdef __cplusplus

#include <obs.h> /* LOG_INFO / LOG_WARNING / LOG_ERROR / LOG_DEBUG enums */
#include <plugin-support.h>

namespace amv {

/* Set / read the project-wide "Detailed logs" flag. The flag is a
 * process-wide std::atomic<bool> so providers and other static-context
 * code can query it cheaply without holding a config pointer.
 *
 * Wiring:
 *   - ConfigManager::load_from_file() calls set_detailed_logs_enabled(...)
 *     after parsing the persisted GlobalSettings.
 *   - The Manager Settings tab Apply handler calls set_detailed_logs_enabled(...)
 *     when the user toggles the checkbox.
 *
 * Default is false: a freshly-installed plugin has the flag off until
 * a config load (which always runs at least once during startup) syncs
 * it to whatever the user picked. */
void set_detailed_logs_enabled(bool enabled);
bool detailed_logs_enabled();

} // namespace amv

/* Detailed-only INFO log. Compiles to a single atomic load on the hot
 * path; the format/varargs side-effects are skipped when the flag is
 * off so we do not pay a snprintf for suppressed lines.
 *
 * Use for periodic / high-frequency diagnostics: [perf] every-5s,
 * [health] media_restart, [health] full recreate, [fill] aspect/snap,
 * VU meters rebuilt summary, provider "created private source" success
 * lines. Anything the user only needs while debugging.
 *
 * For ERROR / WARNING and rare lifecycle INFO (plugin loaded,
 * config saved, scene collection changed, ClearCell), keep using the
 * existing obs_log() directly. */
#define amv_log_detailed(level, ...)                       \
	do {                                               \
		if (amv::detailed_logs_enabled())          \
			obs_log((level), __VA_ARGS__);     \
	} while (0)

#endif /* __cplusplus */
