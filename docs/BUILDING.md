# Building and installing

For the easiest route, use [the guided ISO setup](SETUP.md):
`./setup.sh "/path/to/HALO.iso"`. [Agent-specific instructions](AGENT_SETUP.md)
include a copy/paste prompt. The manual steps below remain available for
existing installations and explicit signing workflows.

## Requirements

- An Apple Silicon Mac, Xcode with the visionOS SDK, and command-line tools.
  The app targets visionOS 26.0. The current runtime was built with the 26.5
  SDK; older toolchain combinations have not been qualified.
- Python 3.12 and the dependencies below. XcodeGen is needed for the Xcode
  project route. The direct SDK build does not require XcodeGen.
- Your own Halo: Combat Evolved for PC retail disc or ISO and a valid product
  key. Install your copy and prepare the supported PC 1.10 executable and
  complete retail game data described below. The Git checkout contains no game
  files; the separate [Complete release](COMPLETE_RELEASE.md) includes static
  game assets and mods. Keys and registration values are never supplied.
- For device installation, your Apple development signing identity, an app
  identifier you control, a provisioning profile containing your Vision Pro,
  and a paired headset with Developer Mode enabled.

```sh
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-development.txt
```

## Supply the executable

Start with your own installed Halo PC copy, obtained from your disc or ISO
using your valid product key. An ISO is installation media; the app does not load it directly.
`setup.sh` can mount it and guide the original installer, or you can supply
an existing installation for the manual steps here. Preparing the retail PC 1.10
installation is a prerequisite to the source-generation steps below.

Generation supports the executable with SHA-256:

```text
c9acf0c469543283cfed6d7dc04ade976dbdfc7cb4532cf070386de169c19545
```

Put your own matching executable at `game/halo.exe` and verify the digest with
`shasum -a 256 game/halo.exe`. Alternatively, set `HALO_EXE` to the absolute
path of your executable and use `shasum -a 256 "$HALO_EXE"`. The `game`
directory is ignored. Address lists are specific to that executable;
Halo Custom Edition, Anniversary, and other builds are not interchangeable.
The manual generation tools do not download or patch game files.
The setup wizard can run your locally supplied retail PC update installer.

## Generate native engine sources

From the repository root:

```sh
python tools/generate_engine_reuse.py \
  @decompilation/c9acf0c46954/function-addresses.txt \
  @decompilation/c9acf0c46954/extra-function-entries.txt \
  --label whole-exe --max-functions 10000 --trap-unsupported --discover --chunks 32
python tools/export_engine_imports.py
```

Inspect `native/build/engine-reuse/whole-exe/generation.json` for generation
errors and unresolved boundaries. Generated code and receipts are local
artifacts and are excluded from the public source package.

## Build

For a native macOS diagnostic host:

```sh
make -C native/EngineHost -j2
```

For an unsigned visionOS application using only the SDK compiler:

```sh
python tools/build_engine_vision.py --configuration Release --direct --clean
```

The output is `native/EngineVision/.build/DirectXROS/HaloVision.app`.
This direct build command does not add game data and is not yet signed for a
headset. The guided setup separately stages a private payload; Xcode includes
that payload when present, and `setup.sh --stage build` adds it to the unsigned
app explicitly.

To generate an editable Xcode project:

```sh
python tools/build_engine_vision.py --generate-only
open native/EngineVision/EngineVision.xcodeproj
```

Set the application's bundle identifier to one you control, select your
team, and enable automatic signing in Xcode. The public default,
`org.example.halovision`, is a placeholder. The CLI normally performs unsigned
builds; an Xcode development run supplies a profile for your account.
The project version is 1.0.3 (build 103). The public source omits the old
screenshot-derived app icon; add your own artwork for distribution.

Both build routes include all verified visual packs. They are stored under
`.setup/VisualMods`, outside Git. Setup downloads the checksum-pinned archive
when necessary; for an offline download run
`python3 tools/visual_assets.py --archive /path/to/MasterChef-v1.0.3-visual-assets.zip`.
The Complete bundle can supply those files directly without another download.

## Supply game data and sign locally

Use a clean directory containing your owned game installation, including
`halo.exe`, `maps`, `shaders`, and `strings.dll`. Copy
`tools/halo-vision-registry.template.txt` to `halo-vision-registry.txt` in that
directory and replace its placeholders with values from your own installation.
The encoded registry values are not the printed product key; do not paste
your product key into the `DigitalProductID` field.
Keep `ExitFlag` set to `clean`. That registry file contains private product
information and must stay outside public source and release uploads.

Build the app with your chosen bundle ID in Xcode. Export that app's signing
entitlements to a local plist and identify your signing identity and device
with Xcode or these read-only commands:

```sh
security find-identity -v -p codesigning
xcrun devicectl list devices
codesign -d --entitlements :- "$PROFILE_APP" > "$ENTITLEMENTS"
```

`PROFILE_APP` is an app signed by Xcode for your team, chosen bundle ID and
headset. `APP` is the compiled app with the same bundle ID. Supply every signing
input explicitly; the tool contains no default personal identity or device.

```sh
python tools/prepare_engine_vision_device.py \
  --app "$APP" --profile-app "$PROFILE_APP" --game "$GAME_DIR" \
  --output native/EngineVision/.build/local-device \
  --team-id "$TEAM_ID" --bundle-id "$BUNDLE_ID" \
  --device-udid "$DEVICE_UDID" --core-device-id "$DEVICE_ID" \
  --identity-sha1 "$IDENTITY_SHA1" --entitlements "$ENTITLEMENTS"
```

Use `--preflight-only` to validate those inputs first. Choose a new output
folder for each package. The tool checks executable identity, file hashes,
profile expiry, device inclusion, signing identity, entitlements and the final
signature. Its output contains your private game files and signing metadata;
it is a local installation package, not the public source release.

With the headset unlocked and on the same network:

```sh
xcrun devicectl device install app --device "$DEVICE_ID" \
  native/EngineVision/.build/local-device/HaloVision.app
xcrun devicectl device process launch --device "$DEVICE_ID" "$BUNDLE_ID"
```

The app imports bundled data into its sandbox. Its emulated user is named
`Player`. Existing development builds that used a different emulated user
folder require an explicit save migration; this source cleanup does not
modify existing installations or saves.

## Optional assets and diagnostics

`tools/import_texmod_pack.py` and `tools/merge_hd_texture_pack.py` support
locally supplied texture packs. Run each with `--help`. No texture or shader
pack binaries are stored in Git; the Complete and visual-assets release ZIPs
carry the published packs. Obtain permissions and retain the relevant notices
for any additional assets you use.

`tools/import_censhine_pack.py` adapts the reviewed 13-program CEnshine 1.0.0
subset to the retail PC shader interface. It takes a locally supplied,
Composer-decoded retail `fx.bin`, the decoded CEnshine collection, and an output
path. It does not download those inputs or include Composer:

```sh
python3 tools/import_censhine_pack.py /path/to/decoded/fx.bin \
  /path/to/decoded/collection /path/to/output/ShaderMods.hvs
python3 tools/test_censhine_import.py
```

This rebuild tool is optional: ordinary setup uses the already verified
release packs. The adapter is also included in `CEnshineSources.zip`, along
with the upstream source/license. See [feature coverage](FEATURES.md).

`tools/probe_engine_menu_input.py` runs bounded desktop diagnostics with a
cloned game directory. `tools/watch_engine_vision_telemetry.py` reads local
headset diagnostics when supplied `--device`, `--bundle-id`, `--build`, and
`--build-id`. Logs and reports can contain paths and device information;
review them before attaching them to public issues.
