# Feature and package coverage

The Build105 development source adds networking, fixes multiplayer menu input
and restores the app icon to the Build91 runtime feature set.
The published v1.0.3/build 103 bundle packages the earlier runtime with
the published assets and setup tools; it does not include multiplayer support.
The table distinguishes included capabilities from completed gameplay
qualification; it is not a claim that every mission or feature is bug-free.

| Capability | Where it is available | Evidence and limits |
| --- | --- | --- |
| Native game execution and Metal rendering | `native/EngineReuse`, `native/EngineHost`, generation/build tools | Fresh engine generation and unsigned Release build passed; generated game code is created locally. |
| Immersive panorama, forward stereo and interface layer | `native/EngineHost/panorama*`, `native/EngineVision/Sources` | Build91 headset startup verified; peripheral panorama is not full stereo and seams remain known issues. |
| Controller input, menus, recentering and haptics | Runtime source and [controls](CONTROLS.md) | Implemented; all controller/headset combinations have not been qualified. |
| Public Internet multiplayer | Build105 source; [multiplayer guide](MULTIPLAYER.md) | Desktop browser retrieved 71 servers; public Timberland join, movement and firing passed. Start-menu navigation, settings, pointer resume and Leave Game passed against a local server. Build104 headset joining is user-reported; populated matches and hosting remain unqualified. Not in v1.0.3 downloads. |
| Audio mixing and interruption recovery | `directsound*`, `audio_session.inc` | Regression checks and an active headset audio queue verified; continuous audible playback remains an acceptance item. |
| Runtime settings and performance options | Settings source and [`mods/runtime-settings.json`](../mods/runtime-settings.json) | The JSON records compiled defaults; it is not a separate settings loader. Per-user tracking/pose data is excluded. |
| Local saves and asset import | `EngineAssets.swift`, `tools/setup_halo.py` | Save-preservation and setup regressions passed. Each recipient creates their own saves and private registration. |
| Game content | Complete release, 41 files | Includes all 32 map/shared-data files, executable, strings, two shader binaries, configuration and four movies; own registration and signing remain required. |
| Visual replacements | Complete and visual-assets releases | Exact published packs contain 1,068 texture entries and 13 shader replacements. Some original textures are intentionally retained. |
| Texture conversion and reviewed image merging | `tools/import_texmod_pack.py`, `tools/merge_hd_texture_pack.py` | Source tools included; local input artwork and its permissions are the user's responsibility. |
| Shader-pack conversion | `tools/import_censhine_pack.py` and `CEnshineSources.zip` | Adapter source, synthetic regression tests and upstream source/license available. Decoded local inputs and Composer are separate prerequisites. |
| Guided installation and agent setup | `setup.sh`, [setup guide](SETUP.md), [agent guide](AGENT_SETUP.md) | ISO and existing-installation paths are documented; humans complete original installer and Apple signing/trust steps. |
| Diagnostics and regression checks | `tools/watch_engine_vision_telemetry.py`, `tools/engine_vision_report_summary.py`, source tests | Tools included; private captures and machine identifiers are excluded. |

## Downloads

Use the [v1.0.3 release](https://github.com/mitchaiet/master-chef/releases/tag/v1.0.3):

- `MasterChef-v1.0.3-Complete.zip`: unsigned app, source, game content, visual
  packs, source/license notices, settings record and per-file manifests.
- `MasterChef-v1.0.3-visual-assets.zip`: the exact texture/shader packs and
  accompanying shader source archive. Source setup downloads and verifies it.
- `MasterChef-v1.0.3-source.zip`: the source snapshot for the tagged release.
  Later source changes are separate from this frozen release download.
- Each ZIP has a matching SHA-256 checksum file.

The Complete app is unsigned and needs private registration/payload preparation
before installation. Follow [the bundle guide](COMPLETE_RELEASE.md); do not
assume a downloaded app can run directly on another headset.

## Remaining work

Combat performance, panorama stitching, texture stability, uninterrupted audio,
extended sessions and full-campaign qualification remain open. See
[known issues](KNOWN_ISSUES.md) and [validation](VALIDATION.md). Hosted GitHub
Actions have been blocked by an account billing restriction; the recorded
local checks are separate from hosted CI.

The package inventory does not establish permission to redistribute third-party
game content or mods. Their ownership and notices remain separate from the MIT
license for original project code; see [asset notices](ASSET_NOTICES.md).
