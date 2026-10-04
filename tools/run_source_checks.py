#!/usr/bin/env python3
"""Small source-only regression suite. No game files, signing or installs; one Metal check draws on the
Mac's GPU when it has one (synthetic shaders)."""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'native/EngineHost'

def generated_engine():
    """The generated engine sources, for checks that run lifted original code:
    HALO_GENERATED_ENGINE, else this checkout's, else the main checkout's when
    this is a worktree. None when there are none; those checks print SKIP."""
    roots = [ROOT]
    try:
        common = subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True).strip()
        roots.append((ROOT / common).resolve().parent)
    except (OSError, subprocess.CalledProcessError):
        pass
    candidates = [os.environ.get('HALO_GENERATED_ENGINE'), os.environ.get('HALO_ENGINE_GEN')]
    candidates += [root / 'native/build/engine-reuse/desktop-build75-direct' for root in roots]
    candidates += [root / 'native/build/engine-reuse/whole-exe' for root in roots]
    return next((Path(c) for c in candidates if c and (Path(c) / 'engine_functions.h').is_file()
                 and (Path(c) / 'sub_004CC0D0.c').is_file()), None)

def run(command):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, check=True, timeout=120)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--portable-only', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    run([sys.executable, ROOT / 'tools/check_repository_hygiene.py'])
    run([sys.executable, ROOT / 'tools/test_repository_hygiene.py'])
    run([sys.executable, ROOT / 'tools/check_probe_runner_exit.py'])
    run([sys.executable, ROOT / 'tools/test_setup_halo.py'])
    run([sys.executable, ROOT / 'tools/test_visual_assets.py'])
    run([sys.executable, ROOT / 'tools/test_censhine_import.py'])
    run([sys.executable, ROOT / 'tools/test_engine_vision_report_summary.py'])
    run([sys.executable, ROOT / 'tools/test_watch_engine_vision_telemetry.py'])
    run([sys.executable, HOST / 'tests/test_dispatch_interest.py'])
    cases = [(name, []) for name in ('frame_pacer', 'panorama_budget', 'pointer_step', 'pointer_input', 'panorama_epoch', 'panorama_gpu_carry', 'panorama_lifecycle', 'panorama_pose_export', 'panorama_camera_cut', 'panorama_motion_budget', 'texture_mips', 'dinput_lifecycle', 'winsock', 'multiplayer_update', 'x87_nearest_fast_path', 'engine_direct_calls', 'controller_link_guard')]
    for name in ('panorama_reentry', 'panorama_projection_hook', 'panorama_letterbox_boundary', 'panorama_lod', 'panorama_interface_record', 'halo_settings_layer_align'):
        cases.append((name, [HOST / 'halo_settings.c']))
    for fast in (0, 1):
        cases.append(('x87_rotating_stack', [HOST / 'tests/x87_stack_shifting.c',
                      f'-DHALO_ARM64_FENV_FAST={fast}', '-frounding-math', '-ffp-contract=off']))
    generated = generated_engine()
    cases.append(('mixer_quality', [HOST / 'directsound_mixer.c', HOST / 'halo_settings.c']))
    cases.append(('texture_mod_pack', []))
    cases.append(('shader_mod_pack', []))
    cases.append(('d3d9_texture_mod', [HOST / 'texture_decode.c']))
    # Differential test against the generated engine (SKIP without it), built as the headset builds the engine.
    cases.append(('visible_surfaces', ['-DHALO_ARM64_FENV_FAST=1', '-frounding-math', '-ffp-contract=off']))
    cases.append(('radial_fog_settings', [HOST / 'halo_settings.c']))
    mojo = ROOT / 'third_party/mojoshader'
    mojo_sources = [HOST / 'metalshader.c', mojo / 'mojoshader.c', mojo / 'mojoshader_common.c',
                    mojo / 'profiles/mojoshader_profile_common.c', mojo / 'profiles/mojoshader_profile_metal.c']
    mojo_flags = ['-DMOJOSHADER_NO_VERSION_INCLUDE=1', '-I', mojo,
                  *(f'-DSUPPORT_PROFILE_{p}=0' for p in ('D3D', 'BYTECODE', 'HLSL', 'GLSL120', 'GLSLES', 'GLSLES3',
                                                       'GLSL', 'ARB1', 'ARB1_NV', 'SPIRV', 'GLSPIRV'))]
    cases.append(('radial_fog', mojo_sources))
    mac = platform.system() == 'Darwin' and not args.portable_only
    # Core telemetry: frame arithmetic, and Halo's own translated tick driver
    # against the game-time counter it reads (SKIP without the translation);
    # on the Mac, guest wait timing and the lifetime pass total too.
    cases += [('core_telemetry', []), ('game_time_telemetry', ['-DHALO_ARM64_FENV_FAST=1', '-frounding-math', '-ffp-contract=off',
                                                               *(['-I', generated] if generated else [])])]
    if mac:
        cases += [('thread_wait_telemetry', []), ('pass_time_telemetry', ['-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip'])]
        cases += [('haptics_mailbox', []), ('audio_watchdog', [HOST / 'directsound_mixer.c']),
                  ('directsound_lifetime', [HOST / 'directsound_mixer.c']),
                  ('directsound_voice_reuse', [HOST / 'directsound_mixer.c']),
                  ('guest_pages', []), ('win32_save_flush', []), ('d3d9_reset', []),
                  ('vb_resident', []), ('draw_indices', []), ('d3d9_radial_fog', []), ('win32_sleep_accounting', [])]
        cases.append(('frame_pacing_host', [HOST / 'halo_settings.c']))
        cases += [(name, []) for name in ('guest_heap', 'win32_heap_reuse', 'd3d9_resource_lifetime', 'd3d9_texture_lifetime',
                                          'thread_cancellation', 'win32_apc_queue', 'win32_tls_lifetime')]
        cases.append(('guest_heap_sound_owners', [HOST / 'directsound_mixer.c']))
        cases += [('guest_thread_qos', []), ('cache_read_wait', [])]
        for name in ('haptics_detector', 'haptics_onset', 'directsound_mixer'):
            extra = [HOST / 'haptics.c', HOST / 'halo_settings.c']
            if name != 'haptics_detector': extra.append(HOST / 'directsound_mixer.c')
            cases.append((name, extra))
    with tempfile.TemporaryDirectory(prefix='halo-source-checks-') as temp:
        for name, extra in cases:
            binary = Path(temp) / name
            flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']
            command = [os.environ.get('CC', 'clang'), *flags, '-pthread', '-DENGINE_FLAT_MEMORY=1', '-I', HOST,
                       '-I', ROOT / 'native/EngineReuse', HOST / 'tests' / f'test_{name}.c', *extra, '-lm']
            if name == 'multiplayer_update' and generated: command += ['-I', generated]
            if name == 'radial_fog':
                command += ['-w', '-frounding-math', '-ffp-contract=off', *mojo_flags]
                # Optional read-only evidence from the local translated engine.
                radial_generated = os.environ.get('HALO_RADIAL_FOG_GENERATED_DIR') or generated
                if radial_generated:
                    command += ['-DRF_HAVE_ENGINE=1', '-I', Path(radial_generated).expanduser().resolve()]
            # Compares against translated engine code, so build it the way the engine is built.
            if name in ('panorama_lod', 'visible_surfaces'):
                command += ['-frounding-math', '-ffp-contract=off', *(['-I', generated] if generated else [])]
            if name in ('audio_watchdog', 'directsound_lifetime', 'directsound_voice_reuse', 'guest_heap_sound_owners'): command += ['-framework', 'AudioToolbox']
            if name == 'panorama_interface_record':
                # Lifted code, built as the engine is; SKIP without the sources.
                command += ['-DHALO_ARM64_FENV_FAST=1', '-frounding-math', '-ffp-contract=off']
                if generated: command += ['-I', generated]
            if name in ('guest_pages', 'win32_save_flush', 'd3d9_reset', 'guest_heap', 'win32_heap_reuse',
                        'd3d9_resource_lifetime', 'd3d9_texture_lifetime', 'd3d9_texture_mod', 'guest_heap_sound_owners', 'win32_apc_queue', 'win32_tls_lifetime',
                        'vb_resident', 'draw_indices', 'd3d9_radial_fog', 'win32_sleep_accounting'):
                command += ['-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip']
            if name == 'cache_read_wait': command += ['-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip']
            run([*command, '-o', binary])
            run([binary])
        # Runtime ABI remains logical ST(i), regardless of the internal ring.
        for engine_dir in ('EngineReuse',):
            binary = Path(temp) / ('runtime-contract-' + engine_dir.replace('/', '-'))
            run([os.environ.get('CC', 'clang'), *flags, '-DENGINE_STEP_FULL=1',
                 '-DHALO_STAGED_ENGINE=1' if engine_dir.startswith('HaloVision') else '-DHALO_STAGED_ENGINE=0',
                 ROOT / 'native/Tests/EngineRuntimeContract.c',
                 ROOT / 'native' / engine_dir / 'engine_runtime.c', '-lm', '-o', binary])
            run([binary])
        run([sys.executable, ROOT / 'tools/benchmark_x87_rotating_stack.py', '--check-only', '--allow-missing',
             *(['--generated', generated] if generated else []), *(['--sanitize'] if args.sanitize else [])])
        # Native leaves against the translated functions, with the release
        # flags and with the headset's (-frounding-math, the FPSR helper).
        engine = generated_engine()
        for variant, extra in (('native_leaves', []), ('native_leaves_headset_flags', ['-frounding-math', '-ffp-contract=off', '-DHALO_ARM64_FENV_FAST=1'])):
            binary = Path(temp) / variant
            flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']
            run([os.environ.get('CC', 'clang'), *flags, *extra, '-DENGINE_FLAT_MEMORY=1', '-I', HOST, '-I', ROOT / 'native/EngineReuse',
                 *(['-I', engine] if engine else []), HOST / 'tests/test_native_leaves.c', '-lm', '-o', binary])
            run([binary])
        # Exact gather loops and the complete translated light/shadow pipeline.
        for name in ('native_gather', 'gather_pipeline'):
            binary = Path(temp) / name
            run([os.environ.get('CC', 'clang'), *flags, '-DENGINE_FLAT_MEMORY=1', '-DHALO_ARM64_FENV_FAST=1',
                 '-frounding-math', '-ffp-contract=off', '-I', HOST, '-I', ROOT / 'native/EngineReuse',
                 *(['-I', engine] if engine else []), HOST / 'tests' / f'test_{name}.c', '-lm', '-o', binary])
            run([binary])
        if mac:
            binary = Path(temp) / 'gather-diagnostics'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 '-import-objc-header', ROOT / 'native/EngineVision/Sources/EngineDiagnosticsBridge.h',
                 ROOT / 'native/EngineVision/Sources/EngineGatherDiagnostics.swift',
                 ROOT / 'native/EngineVision/Tests/GatherDiagnosticsValidation.swift', '-o', binary])
            run([binary])
            # The voice-reuse check again on the original allocator itself
            # (translated 005482E0 and 00547FF0), when a translation is here.
            # Read only; HALO_ENGINE_TRANSLATION names another directory.
            translation = Path(os.environ['HALO_ENGINE_TRANSLATION']) if os.environ.get('HALO_ENGINE_TRANSLATION') else None
            if translation is None:
                found = sorted((ROOT / 'native/build/engine-reuse').glob('*/sub_005482E0.c'), key=lambda f: f.stat().st_mtime)
                translation = found[-1].parent if found else None
            if translation and (translation / 'sub_005482E0.c').exists() and (translation / 'sub_00547FF0.c').exists():
                binary = Path(temp) / 'directsound_voice_reuse_translated'
                run([os.environ.get('CC', 'clang'), *flags, '-pthread', '-DENGINE_FLAT_MEMORY=1', '-DHALO_TRANSLATED_ALLOCATOR',
                     '-I', HOST, '-I', ROOT / 'native/EngineReuse', '-I', translation,
                     HOST / 'tests/test_directsound_voice_reuse.c', HOST / 'directsound_mixer.c',
                     '-framework', 'AudioToolbox', '-lm', '-o', binary])
                run([binary])
            else:
                print('SKIP voice reuse on the translated allocator: no engine translation found', flush=True)
            run([sys.executable, ROOT / 'tools/check_core_telemetry_xros.py'])
            binary = Path(temp) / 'core-telemetry-json'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 '-import-objc-header', ROOT / 'native/EngineVision/Sources/EngineVisionBridge.h',
                 '-Xcc', f'-I{HOST}',
                 ROOT / 'native/EngineVision/Sources/EngineCoreTelemetry.swift',
                 ROOT / 'native/EngineVision/Tests/CoreTelemetryValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'controller-haptics'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 HOST / 'tests/test_controller_haptics.m', '-framework', 'Foundation',
                 '-framework', 'CoreHaptics', '-framework', 'GameController', '-o', binary])
            run([binary])
            binary = Path(temp) / 'controller-snapshot'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 HOST / 'tests/test_controller_snapshot.m', '-framework', 'Foundation',
                 '-framework', 'CoreHaptics', '-framework', 'GameController', '-o', binary])
            run([binary])
            binary = Path(temp) / 'metal-overlay-cache'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 HOST / 'tests/test_metalrenderer_overlay_cache.m', HOST / 'halo_settings.c', '-framework', 'Foundation',
                 '-framework', 'Metal', '-o', binary])
            run([binary])
            # Both draw paths on real Metal: exact framebuffer comparisons and CPU timings.
            binary = Path(temp) / 'metal-draw-fastpaths'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 HOST / 'tests/test_metalrenderer_fastpaths.m', '-framework', 'Foundation',
                 '-framework', 'Metal', '-o', binary])
            run([binary])
            binary = Path(temp) / 'metal-texture-bindings'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 HOST / 'tests/test_metalrenderer_texture_bindings.m', '-framework', 'Foundation',
                 '-framework', 'Metal', '-o', binary])
            run([binary])
            binary = Path(temp) / 'metal-state-pressure'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 HOST / 'tests/test_metalrenderer_state_pressure.m', '-framework', 'Foundation',
                 '-framework', 'Metal', '-o', binary])
            run([binary])
            binary = Path(temp) / 'audio-session-recovery'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks',
                 ROOT / 'native/EngineVision/Tests/AudioSessionRecoveryValidation.m',
                 '-framework', 'Foundation', '-o', binary])
            run([binary])
            # Real Metal and MojoShader with synthetic shaders; two sessions in a temporary cache directory.
            binary = Path(temp) / 'metal-pipeline-cache'
            mojo = ROOT / 'third_party/mojoshader'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks', '-w', '-DMOJOSHADER_NO_VERSION_INCLUDE=1',
                 *(f'-DSUPPORT_PROFILE_{p}=0' for p in ('D3D', 'BYTECODE', 'HLSL', 'GLSL120', 'GLSLES', 'GLSLES3', 'GLSL',
                                                         'ARB1', 'ARB1_NV', 'SPIRV', 'GLSPIRV')),
                 '-I', mojo, HOST / 'tests/test_metalrenderer_pipeline_cache.m', HOST / 'metalshader.c', HOST / 'halo_settings.c',
                 mojo / 'mojoshader.c', mojo / 'mojoshader_common.c', mojo / 'profiles/mojoshader_profile_common.c',
                 mojo / 'profiles/mojoshader_profile_metal.c', '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
            run([binary])
            binary = Path(temp) / 'radial-fog-renderer'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks', '-w', *mojo_flags,
                 HOST / 'tests/test_radial_fog_renderer.m', *mojo_sources, HOST / 'halo_settings.c',
                 '-framework', 'Foundation', '-framework', 'Metal', '-o', binary])
            run([binary])
            binary = Path(temp) / 'immersive-ownership'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 ROOT / 'native/EngineVision/Sources/EngineImmersiveOwnership.swift',
                 ROOT / 'native/EngineVision/Tests/ImmersiveOwnershipValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'panorama-publication'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks', '-DENGINE_FLAT_MEMORY=1',
                 '-I', HOST, '-I', ROOT / 'native/EngineReuse', '-ffunction-sections', '-fdata-sections',
                 ROOT / 'native/EngineVision/Tests/PanoramaPublicationValidation.m',
                 '-framework', 'Foundation', '-framework', 'Metal', '-Wl,-dead_strip', '-o', binary])
            run([binary])
            binary = Path(temp) / 'core-telemetry-collection'
            run([os.environ.get('CC', 'clang'), *flags, '-fobjc-arc', '-fblocks', '-DENGINE_FLAT_MEMORY=1',
                 '-I', HOST, '-I', ROOT / 'native/EngineReuse', '-ffunction-sections', '-fdata-sections',
                 ROOT / 'native/EngineVision/Tests/CoreTelemetryCollectionValidation.m',
                 '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'QuartzCore',
                 '-Wl,-dead_strip', '-o', binary])
            run([binary])
            binary = Path(temp) / 'world-cadence'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 ROOT / 'native/EngineVision/Sources/EngineWorldCadence.swift',
                 ROOT / 'native/EngineVision/Tests/WorldCadenceValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'menu-input'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 ROOT / 'native/EngineVision/Sources/EngineMenuInput.swift',
                 ROOT / 'native/EngineVision/Tests/MenuInputValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'deep-telemetry'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 ROOT / 'native/EngineVision/Sources/EngineDeepTelemetry.swift',
                 ROOT / 'native/EngineVision/Tests/DeepTelemetryValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'diagnostic-history'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 ROOT / 'native/EngineVision/Sources/EngineDiagnosticHistory.swift',
                 ROOT / 'native/EngineVision/Tests/DiagnosticHistoryValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'diagnostic-report-writer'
            run(['swiftc', '-module-cache-path', Path(temp) / 'swift-cache',
                 ROOT / 'native/EngineVision/Sources/EngineDiagnosticHistory.swift',
                 ROOT / 'native/EngineVision/Sources/EngineDiagnosticReportWriter.swift',
                 ROOT / 'native/EngineVision/Tests/DiagnosticReportWriterValidation.swift', '-o', binary])
            run([binary])
            # Layer alignment: the turns against the host's own bearings, then
            # the shipped shader on the Mac's GPU. Optimised: the sphere-wide
            # coverage sweeps are too slow unoptimised.
            alignment = [ROOT / 'native/EngineVision/Sources/EngineImmersiveScreenGeometry.swift',
                         ROOT / 'native/EngineVision/Sources/EngineLayerAlignment.swift']
            binary = Path(temp) / 'layer-alignment'
            run(['swiftc', '-O', '-module-cache-path', Path(temp) / 'swift-cache', *alignment,
                 ROOT / 'native/EngineVision/Tests/LayerAlignmentValidation.swift', '-o', binary])
            run([binary])
            binary = Path(temp) / 'layer-alignment-gpu'
            run(['swiftc', '-O', '-module-cache-path', Path(temp) / 'swift-cache', *alignment,
                 ROOT / 'native/EngineVision/Tests/LayerAlignmentGPUValidation.swift', '-o', binary])
            run([binary, ROOT / 'native/EngineVision/Sources/EngineImmersive.swift'])
    print(f'PASS {len(cases)} C checks and more' + (' plus mock Objective-C, Metal and Swift checks' if mac else '') + '; no headset validation implied')

if __name__ == '__main__':
    main()
