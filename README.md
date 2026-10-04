![master chef — Halo-style silver wordmark over a blue ringworld](docs/assets/master-chef-header.png)

# Master Chef

**Halo: Combat Evolved on Apple Vision Pro.**

An unofficial native port of the original PC game, with a Metal renderer,
immersive panorama, stereo forward view, controller support and haptics.
The project translates the supported Halo PC executable into ARM64 code.

**[Download v1.0.3 — Complete bundle, 1.65 GB](https://github.com/mitchaiet/master-chef/releases/download/v1.0.3/MasterChef-v1.0.3-Complete.zip)**
· [Installation guide](docs/COMPLETE_RELEASE.md)
· [Set up with an agent](docs/AGENT_SETUP.md)
· [Known issues](docs/KNOWN_ISSUES.md)
· [Feature and package coverage](docs/FEATURES.md)

**Playable, experimental release.** Combat frame-rate drops, panorama seams,
texture glitches and audio interruptions remain under investigation. This is
a development build you install through Xcode; your own valid Halo PC
registration and Apple signing are required.

## Features

- Native ARM64 execution with a Metal backend for the game's rendering API.
- A panoramic world with a stereo forward view and a separate game interface.
- Controller movement, aiming, menus, haptics, and reclined recentering.
- Local save storage, bounded diagnostics, and optional local texture packs.
- Hardware texture hashing and bounded reuse of static geometry.
- Public Internet server browsing and joining in the **Build105 development
  source**; [multiplayer setup and test status](docs/MULTIPLAYER.md).

The multiplayer changes require a new source build. They are **not included in
the v1.0.3 download** linked above. Desktop public-server joining, movement and
firing passed, and headset joining has been user-reported. Build105 fixes
multiplayer Start-menu input and restores the app icon.

The Complete bundle includes the unsigned **1.0.3 / build 103** app, game
assets, all **1,068 installed texture replacements and 13 shader replacements**,
matching startup settings, source, notices and checksums. These are the same
visual packs used by Build91; some original textures are intentionally retained.
The Git checkout contains source and documentation; large assets are attached
to the [release](https://github.com/mitchaiet/master-chef/releases/tag/v1.0.3).
Private registration, saves, profiles and Apple signing material are excluded.

## What you need

- Apple Vision Pro running **visionOS 26.0 or later**, with Developer Mode
  enabled, and a paired game controller.
- An **Apple Silicon Mac**, Xcode with the visionOS SDK, and your Apple
  account for signing. The release was built with SDK 26.5.
- **Python 3.12**, **XcodeGen**, and at least **12 GiB free** beyond your
  downloaded/extracted bundle and original media.
- Your own **retail Halo PC license and valid product registration**.
  The ISO route also needs a compatible Wine installation and the retail
  PC 1.10 updater. Importing an existing installation avoids Wine.

Custom Edition, Anniversary, MCC, and Xbox discs are not supported.
See [prerequisites and the Windows fallback](docs/SETUP.md#what-you-need-once).

## Get started

### Complete bundle

Download and extract the Complete ZIP above. If you already have a private
Halo registry export from your registered PC installation, run this inside
the extracted folder:

```sh
./setup.sh --bundled --registry "/path/to/private/halo-install.reg"
```

An existing Wine prefix can supply the registration instead. The
[Complete bundle guide](docs/COMPLETE_RELEASE.md) explains both routes and
download verification. The bundle reuses its included texture/shader packs
without downloading them again. **The included app is unsigned**; setup
prepares your private game payload and Xcode project for installation.

### Start from your retail ISO

If you have the original disc/ISO and printed key, use the guided installer:

```sh
git clone https://github.com/mitchaiet/master-chef.git
cd master-chef
./setup.sh "/path/to/HALO.iso"
```

The wizard mounts your ISO read-only, opens the original installer for your
key, applies your selected retail PC 1.10 updater, imports your game and
private installation values, generates the engine, and opens Xcode with
the game files already included. In Xcode, select your Apple Team, set a
unique bundle ID, choose your Vision Pro and press Run.

The same ISO command works inside the extracted Complete bundle. From a
source clone, setup downloads and verifies the matching visual packs.
You complete the original installer/update and Apple's signing/trust prompts;
this is guided setup, not an unattended installer. Enter your key only in
the original installer, never in chat or a GitHub issue.

- [Guided setup and troubleshooting](docs/SETUP.md)
- **[Set up with a coding agent — copy/paste prompt and dedicated instructions](docs/AGENT_SETUP.md)**
- [Manual build, signing, and installation](docs/BUILDING.md)
- [Controller layout](docs/CONTROLS.md)
- [Online multiplayer](docs/MULTIPLAYER.md)

Check readiness without installing or building:

```sh
./setup.sh "/path/to/HALO.iso" --stage check
```

### Source checks

Source-only checks do not require game files:

```sh
python3 tools/run_source_checks.py --portable-only
```

On an Apple Silicon Mac, run `python3 tools/run_source_checks.py` for the
additional native, Metal, and Swift checks. Optional comparisons against the
original engine report a skip until locally generated code is available.

## Status

Build105 / 1.0.5 development includes native Internet multiplayer, restores
the layered app icon, and fixes controller and pointer input in multiplayer
Start menus. Desktop testing retrieved 71 public servers and joined a
Timberland game with movement and firing. The corrected menu also passed
navigation, settings, pointer resume and Leave Game tests against a local
dedicated server. Build104 headset startup was verified and joining was
user-reported. Build105 is installed and its device-rendered icon is verified;
headset menu acceptance and extended multiplayer qualification remain open.

Build91 was installed and startup-verified on a Vision Pro. Build103 uses the
same runtime fixes and adds complete resource packaging; it has been compiled
and package-verified, but has not had a new full-campaign qualification run.
Local source/build/setup checks passed. See [validation details](docs/VALIDATION.md)
and [known limitations](docs/KNOWN_ISSUES.md) before reporting a problem.

## Feedback and contributions

[Report a bug](https://github.com/mitchaiet/master-chef/issues/new?template=bug_report.yml)
with your version, mission/checkpoint and reproduction steps. For setup help,
include the failed stage and a redacted error. Review attachments before posting;
keep product keys, registry exports, signing files and raw diagnostics private.

Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

## Repository

| Path | Purpose |
| --- | --- |
| `native/EngineHost` | Metal, Windows API, audio, input, and panorama runtime |
| `native/EngineReuse` | Translated execution support and CPU semantics |
| `native/EngineVision` | visionOS app and presenter tests |
| `tools/engine_reuse` | Decoder/lifter adaptations |
| `tools` | Generation, build, local packaging, diagnostics, and checks |
| `third_party` | Required vendored dependencies and licenses |
| `decompilation` | Numeric entry-address lists for the supported executable |

## Licensing and attribution

The original project code is available under the [MIT License](LICENSE).
Third-party licenses and credits are preserved in
[THIRD_PARTY.md](THIRD_PARTY.md) and beside the vendored source.
Game content and visual mods retain their separate rights and
[asset notices](docs/ASSET_NOTICES.md); the MIT license covers original project code.
Halo and its game assets belong to their respective owners. This project is
not affiliated with or endorsed by Microsoft, Bungie, Gearbox, or Apple.
