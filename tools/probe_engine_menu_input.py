#!/usr/bin/env python3
"""Build/run a bounded original-engine menu input probe, with cloned owned data."""
import argparse,ctypes,json,os,pathlib,shutil,subprocess,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('mode',choices=['neutral','dpad','cross','down','enter','back','profile','difficulty','difficulty-key','launch']);p.add_argument('--build',action='store_true')
p.add_argument('--engine-dir',type=pathlib.Path,default=pathlib.Path('native/build/engine-reuse/whole-exe'),help='Generated engine directory, absolute or relative to the repository root (default: canonical whole-exe).')
p.add_argument('--output-dir',type=pathlib.Path,help='Run output directory; old frame captures in this directory are removed before running.')
p.add_argument('--game-source',type=pathlib.Path,default=ROOT/'game',help='Clone this existing game/profile for a diagnostic run, preserving its saves. The source is never modified.')
p.add_argument('--timeout',type=float,default=300,help='Wall-clock timeout in seconds (default: 300).')
p.add_argument('--check-build',action='store_true',help='Check probe freshness without cloning data or launching; combine with --build to rebuild only.')
a=p.parse_args()
if a.timeout<=0:p.error('--timeout must be positive')
engine_dir=a.engine_dir.expanduser();engine_dir=(engine_dir if engine_dir.is_absolute() else ROOT/engine_dir).resolve()
try:engine_label=engine_dir.relative_to(ROOT/'native/build/engine-reuse').name
except ValueError:engine_label=engine_dir.name
base=ROOT/'native/build'/('menu-input-probe' if engine_label=='whole-exe' else f'menu-input-probe-{engine_label}');base.mkdir(parents=True,exist_ok=True)
includes=['-I'+str(x)for x in [ROOT/'native/EngineHost',ROOT/'native/EngineReuse',engine_dir,ROOT/'native/EngineVision/Sources']]
def run(cmd):subprocess.run(list(map(str,cmd)),check=True)
objs=engine_dir/'host-obj'
# Ignore obsolete cached chunks, which do not belong to the current generation.
parts=[objs/(f.stem+'.o')for f in sorted(objs.parent.glob('chunk_*.c'))]
parts += [objs/(name+'.o')for name in ['engine_bundle','engine_imports','host','threading','haptics','halo_settings','pointer','shims_kernel32','shims_misc','winsock','directsound','directsound_mixer','vorbis_shim','d3d9','ddraw','dinput8','gamecontroller','metalrenderer','metalshader','texture_decode','overrides','resources','mojoshader','mojoshader_common','mojoshader_profile_common','mojoshader_profile_metal']]
hostdir=ROOT/'native/EngineHost'
if a.build:
    run(['make','-C',hostdir,'-j2',f'GEN={engine_dir}','halo-host'])
else:
    check=subprocess.run(['make','-s','-q','-C',str(hostdir),f'GEN={engine_dir}','halo-host'])
    if check.returncode:
        p.error('Host objects are stale or cannot be checked. Run this probe with --build; rebuilding halo-host alone does not relink the probe.')
    inputs=parts+[pathlib.Path(__file__),hostdir/'Makefile',
        ROOT/'native/EngineVision/Sources/EngineVisionRuntime.m',
        ROOT/'native/EngineVision/Tests/MenuInputProbe.m']
    for folder in ('native/EngineHost','native/EngineReuse','native/EngineVision/Sources'):
        for pattern in ('*.h','*.inc'):
            inputs.extend((ROOT/folder).glob(pattern))
    probe=base/'probe'
    if not probe.is_file() or any(not f.is_file() or f.stat().st_mtime_ns>probe.stat().st_mtime_ns for f in inputs):
        p.error('Probe is missing or older than its inputs. Run with --build to relink before collecting evidence.')
if a.build:
    for name,src in [('runtime','Sources/EngineVisionRuntime.m'),('main','Tests/MenuInputProbe.m')]:
        path=ROOT/'native/EngineVision'/src
        if name=='runtime':
            contents=path.read_text()
            needle='    os_unfair_lock_unlock(&runtime_lock);\n}\n\nint metalwin_should_close'
            assert contents.count(needle)==1
            contents=contents.replace(needle,'    os_unfair_lock_unlock(&runtime_lock);\n    extern void menu_probe_present(void); menu_probe_present();\n}\n\nint metalwin_should_close')
            # The zero-copy presenter is a different function, and hooking only
            # the byte-copy one meant every probe measurement -- frame timing
            # included -- silently skipped the path the headset actually uses.
            gpu_needle='    mr_commit_async(gpu_published, p); /* always invokes gpu_published exactly once */\n}'
            assert contents.count(gpu_needle)==1
            contents=contents.replace(gpu_needle,'    mr_commit_async(gpu_published, p); /* always invokes gpu_published exactly once */\n    { extern void menu_probe_present(void); menu_probe_present(); }\n}')
            path=base/'runtime_probe.m'
            path.write_text(contents)
        run(['clang','-O2','-fobjc-arc',*includes,'-c',path,'-o',base/(name+'.o')])
    run(['clang','-o',base/'probe',*parts,base/'main.o',base/'runtime.o','-lm','-framework','Foundation','-framework','Cocoa','-framework','Metal','-framework','QuartzCore','-framework','GameController','-framework','CoreHaptics','-framework','AudioToolbox'])
if a.check_build:
    print(json.dumps({'status':'current','probe':str(base/'probe'),'engineDir':str(engine_dir),'rebuilt':a.build}))
    raise SystemExit(0)
out=a.output_dir.expanduser().resolve() if a.output_dir else base/a.mode
out.mkdir(parents=True,exist_ok=True)
if a.output_dir:
    # Only remove the probe's known frame artifacts, preserving game clones and
    # all unrelated content. A reused custom run must not inherit old evidence.
    for pattern in ('frame-*.bgra','frame-*.json'):
        for old in out.glob(pattern):
            if old.is_file():old.unlink()
game=out/'games'/str(time.time_ns())
game.parent.mkdir(exist_ok=True)
game_source=a.game_source.expanduser().resolve(strict=True)
if not game_source.is_dir():p.error('--game-source must be a game directory')
if not game.exists():
    clone=ctypes.CDLL(None,use_errno=True).clonefile
    clone.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int];clone.restype=ctypes.c_int
    def copy(src,dst):
        if clone(os.fsencode(src),os.fsencode(dst),0):shutil.copy2(src,dst)
        return dst
    shutil.copytree(game_source,game,copy_function=copy)
env=dict(os.environ,HALO_FRAME_CAPTURE=str(out))
with (out/'host.log').open('w')as log:
    started=time.time()
    try:result=subprocess.run([str(base/'probe'),str(game),str(['neutral','dpad','cross','down','enter','back','profile','difficulty','difficulty-key','launch'].index(a.mode))],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=a.timeout);code=result.returncode
    except subprocess.TimeoutExpired:code=124
receipt={'mode':a.mode,'gameSource':str(game_source),'gameClone':str(game),'exitCode':code,'outputDir':str(out),'timeoutSeconds':a.timeout,'elapsedSeconds':round(time.time()-started,3),'environment':{k:v for k,v in sorted(env.items()) if k.startswith('HALO_')},'capturedFrames':sorted(f.name for f in out.glob('frame-*.bgra')),'scope':'Original engine with diagnostic input injection; no hardware-input or playable-campaign proof.'}
(out/'run.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt));print('\n'.join((out/'host.log').read_text().splitlines()[-20:]))

# Preserve the recorded child outcome for callers and unattended agents.
# Python reports signal termination as a negative number; use shell convention.
raise SystemExit(code if code >= 0 else 128 - code)
