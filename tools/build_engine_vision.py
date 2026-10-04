#!/usr/bin/env python3
"""Generate and build the standalone whole-engine visionOS application."""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import time

from visual_assets import bundle_visual_assets, ensure_visual_assets

REPO = Path(__file__).resolve().parents[1]
APP = REPO / "native" / "EngineVision"
GEN = REPO / "native" / "build" / "engine-reuse" / "whole-exe"
BUILD = APP / ".build"
PROJECT = APP / "EngineVision.xcodeproj"
DIRECT = BUILD / "DirectXROS"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def generated_inventory() -> dict[str, object]:
    chunks = sorted(GEN.glob("chunk_*.c"))
    required = [GEN / "engine_bundle.c", GEN / "engine_imports.c", GEN / "engine_functions.h"]
    missing = [str(path) for path in required if not path.is_file()]
    if len(chunks) != 32 or missing:
        raise RuntimeError(f"whole-engine source incomplete: chunks={len(chunks)}, missing={missing}")
    bundle = (GEN / "engine_bundle.c").read_text(errors="replace")
    match = re.search(r"ENGINE_FN_COUNT\s*=\s*(\d+)", bundle)
    functions = int(match.group(1)) if match else 0
    return {
        "chunkCount": len(chunks),
        "functionCount": functions,
        "generatedCBytes": sum(path.stat().st_size for path in GEN.glob("*.c")),
        "engineBundleSHA256": sha256(GEN / "engine_bundle.c"),
        "engineImportsSHA256": sha256(GEN / "engine_imports.c"),
    }


def run_stream(command: list[str], log_path: Path, cwd: Path) -> int:
    print("+", " ".join(command), flush=True)
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, bufsize=1)
        assert process.stdout
        for line in process.stdout:
            log.write(line)
            if ("CompileC " in line or "SwiftCompile " in line or "Ld " in line or
                    "BUILD SUCCEEDED" in line or "error:" in line or "warning:" in line):
                print(line.rstrip(), flush=True)
        return process.wait()


def compile_app_icon(product: Path) -> dict[str, object]:
    """Compile the visionOS layered icon and return actool's bundle metadata."""
    partial = product.parent / "app-icon-info.plist"
    partial.unlink(missing_ok=True)
    # A stale catalog must not make an incomplete compile appear successful.
    (product / "Assets.car").unlink(missing_ok=True)
    subprocess.run([
        "xcrun", "actool", str(APP / "Resources/AppIcons.xcassets"),
        "--compile", str(product), "--platform", "xros",
        "--minimum-deployment-target", "26.0", "--target-device", "vision",
        "--app-icon", "AppIcon", "--output-partial-info-plist", str(partial),
        "--warnings", "--errors",
    ], check=True)
    with partial.open("rb") as source:
        info = plistlib.load(source)
    if not (product / "Assets.car").is_file() or not info.get("CFBundleIcons", {}).get("CFBundlePrimaryIcon"):
        raise RuntimeError("App icon compilation did not produce a catalog and primary-icon metadata")
    return info


def direct_xros_build(inventory: dict[str, object], clean: bool, configuration: str) -> tuple[int, Path | None]:
    """Build an unsigned device bundle without Xcode's separately installed platform runtime."""
    if clean:
        shutil.rmtree(DIRECT, ignore_errors=True)
    objects = DIRECT / "objects"
    product = DIRECT / "HaloVision.app"
    objects.mkdir(parents=True, exist_ok=True)
    product.mkdir(parents=True, exist_ok=True)
    icon_info = compile_app_icon(product)
    sdk = subprocess.check_output(["xcrun", "--sdk", "xros", "--show-sdk-path"], text=True).strip()
    target = "arm64-apple-xros26.0"
    includes = [
        "-I", str(APP / "Sources"),
        "-I", str(REPO / "native" / "EngineHost"),
        "-I", str(REPO / "native" / "EngineReuse"),
        "-I", str(GEN),
    ]
    common = ["-target", target, "-isysroot", sdk, f"-ffile-prefix-map={REPO}=.", "-DENGINE_FLAT_MEMORY=1", "-DHALO_ARM64_FENV_FAST=1",
              "-frounding-math", "-ffp-contract=off", "-DMOJOSHADER_NO_VERSION_INCLUDE=1", '-DSUPPORT_PROFILE_D3D=0', '-DSUPPORT_PROFILE_BYTECODE=0', '-DSUPPORT_PROFILE_HLSL=0', '-DSUPPORT_PROFILE_GLSL120=0', '-DSUPPORT_PROFILE_GLSLES=0', '-DSUPPORT_PROFILE_GLSLES3=0', '-DSUPPORT_PROFILE_GLSL=0', '-DSUPPORT_PROFILE_ARB1=0', '-DSUPPORT_PROFILE_ARB1_NV=0', '-DSUPPORT_PROFILE_SPIRV=0', '-DSUPPORT_PROFILE_GLSPIRV=0', "-I", str(REPO / "third_party/mojoshader"), "-O2" if configuration == "Release" else "-O0", *includes]
    host = REPO / "native" / "EngineHost"
    c_sources = [host / name for name in (
        "host.c", "shims_kernel32.c", "shims_misc.c", "winsock.c", "directsound.c", "directsound_mixer.c", "vorbis_shim.c", "d3d9.c", "texture_decode.c", "metalshader.c", "dinput8.c",
        "ddraw.c", "resources.c", "overrides.c", "threading.c", "pointer.c", "haptics.c", "halo_settings.c",
    )]
    c_sources += [REPO / "third_party/mojoshader/mojoshader.c", REPO / "third_party/mojoshader/mojoshader_common.c", REPO / "third_party/mojoshader/profiles/mojoshader_profile_common.c", REPO / "third_party/mojoshader/profiles/mojoshader_profile_metal.c"]
    c_sources += sorted(GEN.glob("chunk_*.c")) + [GEN / "engine_bundle.c", GEN / "engine_imports.c"]
    objc_sources = [APP / "Sources" / "EngineVisionRuntime.m", APP / "Sources" / "EngineDiagnosticsBridge.m", host / "gamecontroller.m",
                    host / "metalrenderer.m"]
    compile_jobs: list[tuple[Path, Path, bool]] = []
    for index, source in enumerate(c_sources + objc_sources):
        compile_jobs.append((source, objects / f"native_{index:03d}_{source.stem}.o", source.suffix == ".m"))
    direct_log = BUILD / "direct-xros-build.log"

    def compile_one(job: tuple[Path, Path, bool]) -> tuple[Path, str]:
        source, output, objective_c = job
        command = ["xcrun", "--sdk", "xros", "clang", *common]
        if objective_c:
            command += ["-fobjc-arc", "-fblocks"]
        command += ["-c", str(source), "-o", str(output)]
        result = subprocess.run(command, cwd=REPO, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"{source}\n{result.stdout}")
        return output, result.stdout

    native_objects: list[Path] = []
    with direct_log.open("w", encoding="utf-8") as log:
        print(f"Direct xros compile: {len(compile_jobs)} native translation units (jobs=2)", flush=True)
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                futures = {pool.submit(compile_one, job): job for job in compile_jobs}
                completed = 0
                for future in concurrent.futures.as_completed(futures):
                    output, diagnostics = future.result()
                    native_objects.append(output)
                    log.write(diagnostics)
                    completed += 1
                    if completed % 8 == 0 or completed == len(compile_jobs):
                        print(f"  native {completed}/{len(compile_jobs)}", flush=True)
        except Exception as error:
            log.write(f"error: {error}\n")
            print(f"direct xros native compile error: {error}", flush=True)
            return 1, None

        swift_sources = sorted((APP / "Sources").glob("*.swift"))
        output_map: dict[str, dict[str, str]] = {}
        swift_objects: list[Path] = []
        for index, source in enumerate(swift_sources):
            output = objects / f"swift_{index:02d}_{source.stem}.o"
            output_map[str(source)] = {"object": str(output)}
            swift_objects.append(output)
        output_map_path = DIRECT / "swift-output-map.json"
        output_map_path.write_text(json.dumps(output_map, indent=2) + "\n")
        swift_command = [
            "xcrun", "--sdk", "xros", "swiftc", "-target", target, "-sdk", sdk,
            "-O" if configuration == "Release" else "-Onone",
            "-parse-as-library", "-swift-version", "5", "-Xfrontend", "-strict-concurrency=minimal",
            "-import-objc-header", str(APP / "Sources" / "EngineVision-Bridging-Header.h"),
            "-Xcc", f"-I{APP / 'Sources'}", "-Xcc", f"-I{host}",
            "-module-name", "EngineVision", "-emit-module", "-emit-module-path",
            str(objects / "EngineVision.swiftmodule"), "-c", *map(str, swift_sources),
            "-output-file-map", str(output_map_path), "-j", "2",
        ]
        print(f"Direct xros Swift compile: {len(swift_sources)} sources (jobs=2)", flush=True)
        swift_result = subprocess.run(swift_command, cwd=REPO, text=True, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
        log.write(swift_result.stdout)
        if swift_result.returncode:
            print(swift_result.stdout, flush=True)
            return swift_result.returncode, None

        executable = product / "HaloVision"
        frameworks = ["Foundation", "SwiftUI", "UIKit", "Metal", "MetalKit", "QuartzCore",
                      "CompositorServices", "ARKit", "GameController", "CoreHaptics", "AudioToolbox", "AVFAudio"]
        link_command = ["xcrun", "--sdk", "xros", "swiftc", "-target", target, "-sdk", sdk,
                        "-o", str(executable), *map(str, sorted(native_objects) + swift_objects),
                        "-Xlinker", "-rpath", "-Xlinker", "@executable_path/Frameworks"]
        for framework in frameworks:
            link_command += ["-framework", framework]
        link_command += ["-Xlinker", "-lm"]
        print("Direct xros link", flush=True)
        link_result = subprocess.run(link_command, cwd=REPO, text=True, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        log.write(link_result.stdout)
        if link_result.returncode:
            print(link_result.stdout, flush=True)
            return link_result.returncode, None

    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "Halo Vision",
        "CFBundleExecutable": "HaloVision",
        "CFBundleIdentifier": "org.example.halovision",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "HaloVision",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0.5",
        "CFBundleSupportedPlatforms": ["XROS"],
        "CFBundleVersion": "105",
        "HaloBuildID": dt.datetime.now(dt.timezone.utc).isoformat(),
        "DTPlatformName": "xros",
        "GCSupportsControllerUserInteraction": True,
        "GCSupportedGameControllers": [{"ProfileName": "ExtendedGamepad"}],
        "GCRequiresControllerUserInteraction": {"visionOS": True},
        "MinimumOSVersion": "26.0",
        "NSLocalNetworkUsageDescription": "Connect to Halo multiplayer servers and players on your local network.",
        "NSHandsTrackingUsageDescription": "A pinch selects the menu item you are looking at.",
        "NSWorldSensingUsageDescription": "Head tracking keeps the curved original-engine screen aligned with your chosen reclined pose; aiming remains on the controller.",
        "UIApplicationPreferredDefaultSceneSessionRole": "CPSceneSessionRoleImmersiveSpaceApplication",
        "UIApplicationSceneManifest": {
            "UIApplicationSupportsMultipleScenes": True,
            "UIApplicationPreferredDefaultSceneSessionRole": "CPSceneSessionRoleImmersiveSpaceApplication",
            "UISceneConfigurations": {"CPSceneSessionRoleImmersiveSpaceApplication": [
                {"UISceneInitialImmersionStyle": "UIImmersionStyleFull"}
            ]},
        },
        "UILaunchScreen": {},
        "UIDeviceFamily": [7],
    }
    info.update(icon_info)
    with (product / "Info.plist").open("wb") as destination:
        plistlib.dump(info, destination, fmt=plistlib.FMT_BINARY)
    (product / "PkgInfo").write_bytes(b"APPL????")
    shutil.copy2(APP / "Resources" / "ThirdPartyNotices.txt", product / "ThirdPartyNotices.txt")
    shutil.copy2(APP / "Resources" / "PrivacyInfo.xcprivacy", product / "PrivacyInfo.xcprivacy")
    bundle_visual_assets(product, ensure_visual_assets())
    if not (product / "HaloVision").is_file():
        return 1, None
    print(f"Direct unsigned xros product: {product}", flush=True)
    return 0, product


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--configuration", default="Debug", choices=["Debug", "Release"])
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--direct", action="store_true", help="Use the SDK compiler directly; produces an unsigned device app")
    args = parser.parse_args()

    for tool in (("xcrun",) if args.direct else ("xcodegen", "xcodebuild", "xcrun")):
        if not shutil.which(tool):
            raise RuntimeError(f"required tool not found: {tool}")
    inventory = generated_inventory()
    ensure_visual_assets()
    BUILD.mkdir(parents=True, exist_ok=True)
    if args.clean:
        shutil.rmtree(BUILD / "DerivedData", ignore_errors=True)
    print(f"Whole engine: {inventory['functionCount']} functions in {inventory['chunkCount']} chunks, "
          f"{inventory['generatedCBytes']:,} generated C bytes", flush=True)

    if args.direct:
        code, product = direct_xros_build(inventory, args.clean, args.configuration)
        print(json.dumps({"exitCode": code, "product": str(product) if product else None, **inventory}, indent=2))
        return code

    generated = subprocess.run(["xcodegen", "generate", "--spec", str(APP / "project.yml"),
                                "--project", str(APP)], cwd=REPO, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(generated.stdout, end="")
    if generated.returncode:
        return generated.returncode
    if args.generate_only:
        print(f"Generated {PROJECT}")
        return 0

    command = [
        "xcodebuild", "-project", str(PROJECT), "-scheme", "EngineVision",
        "-configuration", args.configuration, "-sdk", "xros",
        "-destination", "generic/platform=visionOS", "-derivedDataPath", str(BUILD / "DerivedData"),
        "-jobs", "2", "CODE_SIGNING_ALLOWED=NO", "COMPILER_INDEX_STORE_ENABLE=NO", "build",
    ]
    started = time.time()
    log_path = BUILD / "build.log"
    xcodebuild_result = run_stream(command, log_path, REPO)
    result = xcodebuild_result
    builder = "xcodebuild"
    direct_product: Path | None = None
    log_text = log_path.read_text(errors="replace")
    fallback_reason = None
    if xcodebuild_result != 0 and "platform:visionOS" in log_text and "is not installed" in log_text:
        fallback_reason = "Xcode SDK is installed, but its generic-device platform runtime is absent"
        print(f"xcodebuild unavailable: {fallback_reason}; using direct xros toolchain", flush=True)
        result, direct_product = direct_xros_build(inventory, args.clean, args.configuration)
        builder = "direct-xros-toolchain"
    products = sorted((BUILD / "DerivedData" / "Build" / "Products").glob("**/HaloVision.app"))
    if direct_product:
        products.append(direct_product)
    receipt = {
        "builtAtUTC": dt.datetime.now(dt.timezone.utc).isoformat(),
        "configuration": args.configuration,
        "sdk": subprocess.check_output(["xcrun", "--sdk", "xros", "--show-sdk-version"], text=True).strip(),
        "jobs": 2,
        "optimization": "O2 generated C" if args.configuration == "Release" else "O0 generated C",
        "swiftOptimization": "O" if args.configuration == "Release" else "Onone",
        "exitCode": result,
        "builder": builder,
        "xcodebuildExitCode": xcodebuild_result,
        "xcodebuildFallbackReason": fallback_reason,
        "elapsedSeconds": round(time.time() - started, 3),
        "project": str(PROJECT.relative_to(REPO)),
        "log": str(log_path.relative_to(REPO)),
        "directLog": str((BUILD / "direct-xros-build.log").relative_to(REPO)) if builder == "direct-xros-toolchain" else None,
        "products": [str(path.relative_to(REPO)) for path in products],
        **inventory,
    }
    if result == 0 and products:
        receipt["executableSHA256"] = sha256(products[-1] / "HaloVision")
    (APP / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2), flush=True)
    return result


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"build_engine_vision.py: {error}", file=sys.stderr)
        raise SystemExit(2)
