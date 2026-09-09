#pragma once

/*
OBS Advanced Multiview - shared config-deserialization clamp bounds

Single source of truth for the public clamp boundaries that recur across the
from_obs_data() hardening in the multiview-instance-serialize-*.cpp split. Each
scalar read from a (possibly hand-edited or corrupt) config is clamped to a
defined range; the bounds collected here are the ones shared by more than one
clamp site, so their value lives in exactly one place. Per-struct one-off bounds
(font sizes, margins, dash/thickness pixels, dB levels, ...) stay local to their
own from_obs_data().

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#include <cstddef>
#include <cstdint>

namespace amv::limits {

/* Output/compose dimension (px). A huge or zero dimension feeds straight into
 * gs_texrender_create and the GPU shared texture, so every resolved dimension is
 * clamped to [kMinDim,kMaxDim]: the lower bound matches the dialog spinbox (16),
 * the upper (16384, >8K) stays well within texture-size limits. */
inline constexpr uint32_t kMinDim = 16;
inline constexpr uint32_t kMaxDim = 16384;

/* Persisted image / file-path string length (bytes); bounds a pathological
 * string from a manually edited config. */
inline constexpr size_t kMaxImagePathLen = 4096;

/* Persisted font-family string length (bytes); bounds a pathological string that
 * would otherwise break Qt font enumeration. */
inline constexpr size_t kMaxFontFamilyLen = 128;

/* OBS mixer audio tracks are numbered 1..6. */
inline constexpr int kMinAudioTrack = 1;
inline constexpr int kMaxAudioTrack = 6;

/* Normalized opacity / ratio, clamped to [0,1]. */
inline constexpr double kMinOpacity = 0.0;
inline constexpr double kMaxOpacity = 1.0;

} // namespace amv::limits
