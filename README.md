# OBS Advanced Multiview

OBS Advanced Multiview is a highly configurable multiview plugin for OBS Studio. It is built for operators who need more than the built-in Multiview: custom grids, scene and source cells, audio-only monitoring, visual overlays, safe areas, VU meters, lost-signal handling, and external signal providers.

[简体中文](README.cn.md)

## Features

- Multiple independent multiview windows, each with its own layout and settings.
- Custom grid layouts with row/column counts, merged cells, and configurable gutter spacing.
- Internal OBS cells: Program, Preview, scenes, sources, and audio-only sources.
- External signal cells:
  - FFmpeg media URLs and local files
  - DistroAV NDI sources
  - obs-spout2 Spout senders
  - VLC playlists
  - WebRTC placeholder for future support
- Global, instance, and cell-level visual settings:
  - background fill and images
  - labels
  - safe area guides
  - VU meters
  - overlays
  - PGM/PRVW highlight borders
- Signal-lost behavior for internal and external sources:
  - missing-source overlays
  - placeholder and signal-lost images
  - reconnect and replay actions
  - fallback modes
- Scene-click switching: click a scene cell to send it to Preview in Studio Mode, or directly to Program outside Studio Mode.
- English and Simplified Chinese UI localization.

## Requirements

- OBS Studio 31.1.1 or newer.
- Windows is the primary tested platform.
- Optional host plugins for external providers:
  - DistroAV for NDI cells
  - obs-spout2 for Spout cells
  - OBS VLC source support for VLC playlist cells

macOS and Linux support is planned through the cross-platform build system, but current validation is Windows-first.

## Installation

Download a release archive from GitHub Releases.

For OBS portable installs, use the portable archive and extract it into the OBS root folder so the final layout contains:

```text
obs-plugins/64bit/obs-advanced-multiview.dll
data/obs-plugins/obs-advanced-multiview/locale/en-US.ini
data/obs-plugins/obs-advanced-multiview/locale/zh-CN.ini
```

Restart OBS, then open the plugin from:

```text
Tools -> OBS Advanced Multiview
```

For development or local testing, the deployment script can copy the latest build into configured OBS portable folders:

```powershell
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
```

## Quick Start

1. Open `Tools -> OBS Advanced Multiview`.
2. Create an instance.
3. Set rows, columns, merged cells, and gutter spacing.
4. Right-click a cell in the multiview window and choose `Add Source...`.
5. Use `Cell Display Settings...`, `Signal Lost Settings...`, and `Instance Visual Settings...` to tune the presentation.

## Build From Source

The project uses the OBS plugin template build system, CMake, Qt 6, C++17, and the OBS frontend API.

On Windows:

```powershell
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo --target obs-advanced-multiview
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
```

See [docs/setup/README.md](docs/setup/README.md) for first-time setup and troubleshooting.

## Documentation

- [Development workflow](docs/DEVELOPMENT.md)
- [Setup guide](docs/setup/README.md)
- [Distribution notes](docs/setup/DISTRIBUTION.md)
- [Roadmap](docs/ROADMAP.md)
- [Known limitations](docs/known-limitations.md)
- [Terminology](docs/TERMINOLOGY.md)

Design and implementation notes are kept under [docs](docs/). The historical project plan has been moved to [docs/ROADMAP.md](docs/ROADMAP.md).

## Current Status

The 1.0 release candidate focuses on Windows operation, custom multiview layouts, internal OBS source monitoring, external media/NDI/Spout/VLC providers, signal-lost handling, visual customization, and bilingual English / Simplified Chinese UI.

## License

This project is licensed under GPL-2.0-or-later. See [LICENSE](LICENSE).