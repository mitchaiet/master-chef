# Changelog

## 1.0.5 / Build105 development

- Restores the layered app icon in both direct and Xcode builds, including
  the compiled asset catalog and primary-icon bundle metadata.
- Fixes controller and pointer input after opening Start during multiplayer.
  The active menu root now owns input even when the game does not pause.
- Verifies navigation, settings, pointer resume and Leave Game against a
  local retail PC 1.10 dedicated server. Adds a reproduced input regression
  and exact-byte checks for the restored icon layers.

## 1.0.4 / Build104 development

- Adds native IPv4 Winsock compatibility for public Internet server browsing
  and joining, and replaces the obsolete Windows updater check with the
  original game's no-update result for the required PC 1.10 executable.
- Verifies desktop public-server browsing, joining, movement and firing.
  Headset installation and startup passed; successful headset joining was
  subsequently user-reported. See [validation](docs/VALIDATION.md).

These development changes are not included in the v1.0.3 downloads.

## 1.0.3

- Publishes the full visual selection used by Build91: 1,068 texture and 13
  shader replacements, with original CEnshine shader source and license.
- Fixes both direct and Xcode builds omitting the texture/shader packs.
  Setup fetches checksum-pinned packs; the Complete bundle can reuse its
  included copies without a second download.
- Preserves the original configuration and movie files when importing game
  data; personal saves and profiles remain excluded.
- Adds archive integrity, safe extraction, offline bundle, tamper and
  preservation regressions. Unknown or modified packs are never silently used.
- Adds a fresh unsigned visionOS build (103), full release manifests and
  setup instructions. Product registration and Apple signing remain local.
- Corrects the generated header's E/F lettering and records verified Build91
  headset installation/startup. Runtime gameplay code is unchanged from 1.0.2.

## 1.0.2

- Supports independent U/V texture addressing, mirrored repetition and mirrored
  clamping instead of rejecting those draws.
- Reuses bounded sampler and depth/stencil caches under pressure instead of
  permanently refusing new material states. Inactive stencil settings share
  one cache entry; failed state allocations remain retryable.
- Retries failed foreground audio activation, queue rebuilds and queue starts
  every two seconds. Recovery respects backgrounding, interruptions, denied
  resume permission and stopped runtime state.
- Adds real-Metal pixel and cache-pressure regressions plus deterministic audio
  lifecycle failure tests.
- Adds the generated Master Chef header, refreshed release documentation and
  a checksum-restricted documentation artwork exception in the hygiene audit.
- Runs source CI on macOS, matching the Mach APIs and Apple linker used by
  host fixtures. Probe freshness checks now include implementation includes.

These fixes are shared with the owner Build91. They do not establish stable
FPS, uninterrupted audible playback or a complete campaign playthrough.

## 1.0.1

- Adds `setup.sh` for guided retail ISO installation, existing-install imports,
  private registry conversion, dependency setup, generation and Xcode handoff.
- Bundles locally owned game files into the generated Xcode project and enables
  automatic signing with the user's account.
- Adds readiness reports, disk checks, resumable setup, and setup regressions.
- Adds a dedicated agent setup guide and Windows-prepared installation fallback.
- Keeps the original installer/key entry, patch selection and Apple signing as
  explicit human steps; no game files or keys are distributed.

## 1.0.0

First curated public source snapshot of the native visionOS runtime.
Original project code is released under the MIT License.

- Includes the current Metal renderer, panorama scheduler, controller and
  audio integration, ARM texture CRC path, and bounded geometry reuse.
- Includes source-only C, Objective-C, Metal, Swift, and Python checks.
- Moves the pinned decoder/lifter dependency to `third_party/xwa`.
- Parameterizes device and signing inputs and uses a generic emulated player.
- Replaces development handoffs with build, controls, architecture, and
  limitations documentation.
- Excludes historical Git data, prototype applications, local reports,
  generated engine code, game assets, private signing data, and binaries.

This version identifies the source distribution. See docs/KNOWN_ISSUES.md for
runtime limitations and docs/VALIDATION.md for the release checks actually run.
