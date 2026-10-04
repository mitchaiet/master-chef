# Host runner for the statically translated Halo executable (macOS arm64).
VISION := $(abspath ../..)
GEN    := $(VISION)/native/build/engine-reuse/whole-exe
OBJ    := $(GEN)/host-obj
CC     := clang
CFLAGS := -O2 -w -std=c11 -DENGINE_FLAT_MEMORY=1 -DHALO_ARM64_FENV_FAST=1 -I. -I$(VISION)/native/EngineReuse -I$(GEN)
CHUNKS := $(wildcard $(GEN)/chunk_*.c)
CHUNK_OBJS := $(patsubst $(GEN)/%.c,$(OBJ)/%.o,$(CHUNKS))
.DEFAULT_GOAL := all
# Generated chunks include individual lifted functions. Track those inputs even
# for cached objects created before dependency files were introduced.
$(foreach chunk,$(CHUNKS),$(eval $(OBJ)/$(notdir $(chunk:.c=.o)): $(addprefix $(GEN)/,$(shell sed -n 's/^.*"\(sub_[^"]*\.c\)".*/\1/p' $(chunk)))))
HOST_SRCS := host.c threading.c haptics.c halo_settings.c pointer.c shims_kernel32.c shims_misc.c winsock.c d3d9.c texture_decode.c metalshader.c dinput8.c ddraw.c directsound.c directsound_mixer.c vorbis_shim.c resources.c overrides.c main.c
HOST_OBJC := metalwin.m metalrenderer.m gamecontroller.m
MOJO_DEFINES := -DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_GLSL120=0 -DSUPPORT_PROFILE_GLSLES=0 -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_GLSL=0 -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0
MOJO_OBJS := $(OBJ)/mojoshader.o $(OBJ)/mojoshader_common.o $(OBJ)/mojoshader_profile_common.o $(OBJ)/mojoshader_profile_metal.o
HOST_OBJS := $(patsubst %.c,$(OBJ)/%.o,$(HOST_SRCS)) $(patsubst %.m,$(OBJ)/%.o,$(HOST_OBJC))

all: halo-host

$(OBJ)/overrides.o: campaign_unlock.h visible_surfaces.h multiplayer_update.h $(VISION)/native/EngineReuse/engine_flags.h
$(OBJ)/pointer.o: pointer_step.inc
$(OBJ)/d3d9.o: texture_mod_runtime.inc texture_mod_pack.h shader_mod_runtime.inc shader_mod_pack.h

$(GEN)/engine_bundle.c:
	cd $(VISION) && python3 tools/generate_engine_reuse.py @decompilation/c9acf0c46954/function-addresses.txt @decompilation/c9acf0c46954/extra-function-entries.txt --label whole-exe --max-functions 10000 --trap-unsupported --discover --chunks 32
$(GEN)/engine_imports.c: $(VISION)/tools/export_engine_imports.py
	cd $(VISION) && python3 tools/export_engine_imports.py

$(OBJ)/%.o: $(GEN)/%.c $(VISION)/native/EngineReuse/engine_hooks.h $(VISION)/native/EngineReuse/engine_cpu.h $(VISION)/native/EngineReuse/engine_arm64_fenv_prototype.h $(VISION)/native/EngineReuse/engine_flags.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/%.o: %.c host.h $(VISION)/native/EngineReuse/engine_hooks.h $(VISION)/native/EngineReuse/engine_cpu.h $(VISION)/native/EngineReuse/engine_arm64_fenv_prototype.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ)/metalwin.o: metalwin.m metalwin.h | $(OBJ)
	$(CC) -O2 -w -ObjC -fobjc-arc -c metalwin.m -o $@
$(OBJ)/gamecontroller.o: gamecontroller.m gamecontroller.h | $(OBJ)
	$(CC) -O2 -ObjC -fobjc-arc -c gamecontroller.m -o $@
$(OBJ)/mojoshader.o: $(VISION)/third_party/mojoshader/mojoshader.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJO_DEFINES) -I$(VISION)/third_party/mojoshader -c $< -o $@
$(OBJ)/mojoshader_common.o: $(VISION)/third_party/mojoshader/mojoshader_common.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJO_DEFINES) -I$(VISION)/third_party/mojoshader -c $< -o $@
$(OBJ)/mojoshader_profile_%.o: $(VISION)/third_party/mojoshader/profiles/mojoshader_profile_%.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJO_DEFINES) -I$(VISION)/third_party/mojoshader -c $< -o $@
$(OBJ)/metalshader.o: metalshader.c metalshader.h | $(OBJ)
	$(CC) $(CFLAGS) -DMOJOSHADER_NO_VERSION_INCLUDE=1 -c $< -o $@
$(OBJ)/metalrenderer.o: metalrenderer.m metalrenderer.h metalshader.h | $(OBJ)
	$(CC) -O2 -ObjC -fobjc-arc -c metalrenderer.m -o $@
$(OBJ)/host.o: host_snapshot.h
$(OBJ)/overrides.o: a10_control.inc a10_gamepad.inc hsc_trace.inc gamecontroller.h panorama_hooks.inc panorama.h panorama_overlay_scope.h panorama_budget.h native_gather.h native_gather_tree.h gather_diagnostics.h
$(OBJ)/host.o $(OBJ)/shims_kernel32.o $(OBJ)/threading.o: threading.h
$(OBJ)/dinput8.o: gamecontroller.h
$(OBJ)/d3d9.o: stateblock.h d3d9_render.inc d3d9_capture.inc fp_vertex_capture.inc d3d9_process_vertices.inc process_vertices.h vertex_shader_cpu.h rhw_declaration.h panorama_render.inc panorama.h metalrenderer.h texture_decode.h gamecontroller.h texture_content_hash.h third_party/xxhash/xxhash.h
$(OBJ):
	mkdir -p $(OBJ)

halo-host: $(CHUNK_OBJS) $(OBJ)/engine_bundle.o $(OBJ)/engine_imports.o $(HOST_OBJS) $(MOJO_OBJS)
	$(CC) -O2 -o $@ $^ -lm -framework Cocoa -framework Metal -framework QuartzCore -framework GameController -framework CoreHaptics -framework AudioToolbox

clean:
	rm -rf $(OBJ) halo-host
.PHONY: all clean

$(OBJ)/d3d9.o: model_vertex_capture.inc model_capture_scope.h model_capture_boundary.h
$(OBJ)/overrides.o: model_capture_hooks.inc model_capture_boundary.h
$(OBJ)/overrides.o: native_leaves.h $(VISION)/native/EngineReuse/engine_flags.h

$(OBJ)/d3d9.o: process_vertices_skin.h process_vertices_differential.h
