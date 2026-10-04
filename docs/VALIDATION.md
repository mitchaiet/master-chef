# Source-release validation

## Build104 multiplayer development

Validated on October 4, 2026, on an Apple Silicon Mac:

- The guest Winsock bridge passed native UDP/TCP peer tests, both DLL ordinal
  layouts, DNS structure translation, socket errors, nonblocking connect and
  select, datagram truncation, timeouts, stale handles, concurrent close,
  cleanup and thread cancellation. Focused ASan/UBSan checks also passed.
- The native app update policy matched all guest memory written by the
  original Halo PC 1.10 no-update callback. Dispatch and direct-call hook
  checks passed. Registration and server compatibility checks remain intact.
- A local unmodified retail PC 1.10 dedicated server accepted a direct-IP
  connection and reported one player on Blood Gulch. The client rendered
  700 frames. Distinct client/server UDP ports were used on the same Mac.
- The original Internet browser retrieved 71 servers and reported 21 players
  online. A public Timberland CTF server accepted a join. Controller input
  moved the player's origin approximately 4.2 game units; firing reduced the
  loaded magazine from four rounds to three. The session advanced more than
  9,500 game ticks between the initial and final gameplay snapshots.
- The desktop host, input probe and full ARM64 visionOS Release app compiled.
  The visionOS build compiled 60 C/Objective-C units and 24 Swift sources,
  using an existing private translation of the verified 1.10 executable.
  This was not a fresh engine-generation test. The existing visual packs
  matched the published checksums.
- The portable source suite passed from a source-only snapshot (34 C targets
  plus Python/runtime checks; optional engine comparisons skipped). The new
  update-policy comparison also passed with the private generated callback.
  Source hygiene passed. A broader run with that existing translation hit
  `panorama_interface_record`'s unexpected dispatch at `00511DF0`; unchanged
  public main reproduces the same failure. The separate legacy desktop-video
  fixture also fails on unchanged main because its expected display dimensions
  predate the runtime's current values. Neither fixture was changed here.

These are desktop connection and input smoke tests. The selected public server
was empty before joining, so combat against another player and replication
quality were not tested. Headset installation, headset networking, sustained
multiplayer performance, hosting and extended sessions remain pending. The
v1.0.3 release files have not been replaced by this development build.

## Source completeness follow-up

The comparison with the development tree covered 271 public native files.
Of those, 260 are byte-identical; the other 11 have reviewed
anonymization, source portability, notices, signing or resource-packaging
differences. No missing runtime fix was found in this comparison.

The CEnshine shader adapter already present in the release's `CEnshineSources.zip` is
now also available as `tools/import_censhine_pack.py`, with four synthetic
regressions registered in the source suite. Those four checks, the 24 setup
regressions and nine visual-asset/content checks passed. Source hygiene and
secret scans passed. No runtime behavior, release binary or existing ZIP
checksum changed during this follow-up; it is not a new headset acceptance run.

## 1.0.3 complete packaging update

- Fresh public-source engine generation completed with 8,336 functions and
  32 chunks. An unsigned ARM64 visionOS Release build compiled 59 native units
  and 24 Swift sources, version 1.0.3/build 103, SDK 26.5.
- The direct app contains the exact Build91 texture and shader packs: 1,068
  texture entries and 13 shader replacements. All pack hashes match the
  installed baseline. The CEnshine source/license archive is also included.
- Xcode project generation confirms every visual pack and manifest in Copy
  Bundle Resources. Previously these resources were omitted by public builds.
- All nine visual-asset/content tests passed: exact import/resume/bundling, outer
  checksum rejection, changed-pack preservation, unsafe and symbolic entry
  rejection, invalid inner content, symbolic destination rejection and local
  Complete-bundle reuse without a network request, plus preservation of the
  original configuration and movie assets while excluding saves/registration.
  The 24 setup regressions
  and portable source suite (32 C targets plus additional checks) also passed.
- The renderer, audio recovery, native settings and settings UI sources match
  the frozen Build91 sources. The sampled headset preferences contained no
  saved resolution override; the recorded startup resolution was 2048x1536.
  Device tracking history and physical pose are not copied into the release.

The new build is package-verified and unsigned. Build91 installation and
startup evidence below remains the headset evidence for these runtime fixes;
it does not prove a new Build103 installation, full mission performance or
audible continuity. Other users supply their own registration and signing.

## 1.0.2 rendering and audio recovery update

Validated on an Apple Silicon Mac:

- Full source suite: 59 C checks plus Objective-C, Metal, Swift and Python
  checks passed. The portable subset also passed normally and with ASan/UBSan
  (32 C checks). Optional comparisons requiring generated engine code were
  skipped in the public checkout.
- Real-Metal regressions reproduced the previous sampler/depth cache failures
  and the rejected mixed texture address modes. The fixes passed 768 sampler
  requests and 192 depth/stencil states, including actual queued draws beyond
  both cache capacities. Cache storage remained bounded.
- All 25 U/V address combinations passed nine out-of-bounds pixel comparisons
  with each renderer path: 450 exact GPU pixel checks. Nonzero border colors
  remain unsupported.
- The production audio coordinator passed injected activation, rebuild and
  queue-start failures, two-second retry timing, background/interruption
  suppression, denied resume permission and stopped-runtime checks. The old
  coordinator reproduced the permanent-suspension failure. The new renderer
  and audio tests also passed focused ASan/UBSan runs. Audio framework calls
  in this lifecycle test are mocked; this is not an audible-output test.
- All 24 setup regressions and four documentation-art hygiene regressions
  passed. The reviewed header requires an exact path and SHA-256 match.

The corresponding owner Build91 was compiled from a frozen private snapshot
with fresh objects: 59 native units, 32 translated chunks and 24 Swift sources.
Signing and the complete local game-payload manifest passed. That bundle is
private. The public source app uses version 1.0.2/build 102, a separate build
number series, and requires the user's game files and signing.

Desktop entry probes for Halo (a30) and The Maw (d40) each completed 2,400 frames
with scripted controller turns and normal audio mixing enabled. All ten
captured GPU layers were reviewed for each scene. Median measured entry-scene
rates were 29.78 and 29.86 FPS respectively. These short desktop probes do not
measure firefights, full missions, audible output or headset performance, and
do not establish an improvement over an equivalent baseline.

The initial owner installation attempt could not reach the headset. A later
attempt on September 29, 2026 (Central time) installed and launched Build91;
device inventory independently confirmed version 1.0.2/build 91. Five startup
diagnostic snapshots matched the expected BuildID. The final snapshot reached
engine frame 1,226 with immersive presentation active, 2,708 completed GPU
frames and zero GPU failures. Its audio queue was running with nonzero samples,
zero enqueue failures and zero watchdog rebuilds. This verifies installation
and startup, not audible continuity, visual acceptance or campaign performance.
Known limitations remain in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

CI now targets macOS because the host fixtures depend on Mach APIs and Apple's
linker. The local passes above are not hosted CI results; previous hosted runs
were blocked by the account's billing status. Source publication and device
acceptance remain separate gates.

## 1.0.1 setup update

- Passed 24 synthetic setup regressions: Windows/Wine registry parsing,
  private-value error handling, owner-only files, supported-file import,
  rollback, resume, preservation of changed files, payload manifests,
  low-space refusal, installer refusal without a human terminal, and ISO
  cleanup after failure/cancellation.
- Passed the portable source suite on an Apple Silicon Mac, including the
  new setup regressions.
- Checked a real owned retail ISO with `--stage check --json`: the layout was
  recognized, the original executable was identified as needing the PC 1.10
  update, and the temporary read-only mount was detached afterward.
- Verified XcodeGen with a synthetic local payload: both payload resources
  appear in Copy Bundle Resources and automatic signing is enabled.
- The current validation Mac had less than the required 12 GiB free. The
  preflight correctly reported this. A fresh Wine installer/key-entry/update
  session and the complete ISO-to-headset path were **not** run for this
  update. The Windows fallback export instructions were not exercised on a
  Windows machine. No new signed build or headset acceptance is claimed.

The earlier hosted GitHub Actions attempts were blocked by the account's
billing status before tests could start. Local test results above are not
hosted Linux CI results. Original runtime code is unchanged by this setup
update; app metadata is now 1.0.1 (build 101).

## 1.0.0 runtime/source baseline

The following checks were completed from the curated source tree on an
Apple Silicon Mac. Local game data was used only for generation and optional
reference comparisons; no game data or resulting binaries are distributed.

| Check | Result |
| --- | --- |
| Full source suite | Passed: 59 C targets plus additional Objective-C, Metal, Swift, and Python checks |
| Portable subset with ASan/UBSan | Passed: 32 C targets plus additional comparisons |
| Save preservation after generic player-path cleanup | Passed: reuse, missing-file detection, save-tree preservation, failed-promotion rollback |
| Fresh engine generation | 8,336 functions in 32 translation units; no unresolved function frontier |
| Unsigned visionOS Release compile and link | Passed: 59 native units and 24 Swift sources, ARM64, SDK 26.5, deployment 26.0 |
| Public version metadata | 1.0.0, build 100; no personal bundle, team, or device ID |
| Xcode project generation | Passed |
| Required signing/device inputs | Missing inputs rejected; no personal defaults |
| Source path/content hygiene | Passed |
| Gitleaks 8.30.1 with decoding | No unreviewed findings |
| Source ZIP integrity | Every archived file checked against the included SHA-256 manifest |

Two Gitleaks false positives were reviewed in unchanged xxHash AVX512
arithmetic. The checked-in configuration suppresses only those exact
expressions in that specific file. Third-party license notices are retained.
The privacy audit includes personal home paths, device/signing identifiers,
private-network addresses, secret patterns, binary artifacts, and archive
entry names. No automatic scan can guarantee discovery of every possible
secret; this export also uses an explicit source selection and manual review.

The full suite ran before generated code was present and reported skips for
optional original-engine comparisons. The later sanitizer subset ran with
freshly generated code, including native light/shadow gather comparisons.
A real-map gather test was skipped because no map fixture was supplied. The
portable subset was executed on macOS; the included Ubuntu CI workflow has
not yet run on a hosted runner for this release.

The generator records 15 undecodable entries and 47,843 explicit instruction
trap annotations in the discovered executable regions. These counts do not
establish whether the corresponding paths are reachable in gameplay. Keep
checking the generation receipt when changing the supported executable or
translator; unresolved instructions fail explicitly at runtime.

The freshly generated application was compiled and linked, but this public
source revision was not signed, installed, or played on a headset. No claim
of full-campaign completion, stable combat FPS, or resolved visual/audio
issues follows from these checks. See KNOWN_ISSUES.md.

Generated sources, app products, intermediate objects, project files, raw
reports, device data and private Git history were removed from the release
tree after validation. SOURCE_MANIFEST.json lists the source archive contents
except for the manifest itself; the external checksum covers the whole ZIP.
