/* Host runtime for the statically translated Halo executable.
   One flat 4 GiB guest address space; Win32 imports are served by native shims. */
#ifndef HALO_HOST_H
#define HALO_HOST_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define ENGINE_FLAT_MEMORY 1
#include "engine_cpu.h"

typedef struct { const char *name; uint32_t va, virtual_size, raw_offset, raw_size, characteristics; } EngineSection;
typedef struct { uint32_t iat_va; const char *dll, *name; } EngineImport;
extern const uint32_t engine_pe_entry_point, engine_pe_image_base, engine_pe_size_of_image, engine_pe_tls_directory;
extern const EngineSection engine_pe_sections[]; extern const size_t engine_pe_section_count;
extern const EngineImport engine_imports[]; extern const size_t engine_import_count;

typedef void (*HostShim)(EngineCPU *cpu);
typedef struct { const char *dll, *name; HostShim fn; } HostShimEntry;
extern const HostShimEntry host_shims_kernel32[];
extern const HostShimEntry host_shims_misc[];
extern const HostShimEntry host_shims_winsock[];
void host_winsock_shutdown(void);

/* Guest memory access. */
extern uint8_t *engine_flat_base;
#define GPTR(a)  ((void *)(engine_flat_base + (uint32_t)(a)))
#define GSTR(a)  ((const char *)(engine_flat_base + (uint32_t)(a)))
static inline uint8_t  G8(uint32_t a)  { return engine_flat_base[a]; }
static inline uint16_t G16(uint32_t a) { uint16_t v; memcpy(&v, engine_flat_base + a, 2); return v; }
static inline uint32_t G32(uint32_t a) { uint32_t v; memcpy(&v, engine_flat_base + a, 4); return v; }
static inline uint64_t G64(uint32_t a) { uint64_t v; memcpy(&v, engine_flat_base + a, 8); return v; }
static inline void S8(uint32_t a, uint8_t v)   { engine_flat_base[a] = v; }
static inline void S16(uint32_t a, uint16_t v) { memcpy(engine_flat_base + a, &v, 2); }
static inline void S32(uint32_t a, uint32_t v) { memcpy(engine_flat_base + a, &v, 4); }
static inline void S64(uint32_t a, uint64_t v) { memcpy(engine_flat_base + a, &v, 8); }

/* Merged physical and menu-only controller keyboard state, sampled on the game thread. */
void host_dinput_keyboard_state(uint8_t out[256]);

/* Shim calling helpers: stdcall args at [esp+4], [esp+8], ... */
#define ARG(i) G32(cpu->gpr[4] + 4u + 4u * (uint32_t)(i))
#define RET_STDCALL(v, n) do { uint32_t rv_ = (uint32_t)(v); uint32_t ra_ = engine_pop(cpu, 4); cpu->gpr[4] += 4u * (uint32_t)(n); cpu->gpr[0] = rv_; cpu->pc = ra_; return; } while (0)
#define RET_CDECL(v) do { uint32_t rv_ = (uint32_t)(v); uint32_t ra_ = engine_pop(cpu, 4); cpu->gpr[0] = rv_; cpu->pc = ra_; return; } while (0)
#define RET64_STDCALL(v, n) do { uint64_t v64_ = (uint64_t)(v); uint32_t ra_ = engine_pop(cpu, 4); cpu->gpr[4] += 4u * (uint32_t)(n); cpu->gpr[0] = (uint32_t)v64_; cpu->gpr[2] = (uint32_t)(v64_ >> 32); cpu->pc = ra_; return; } while (0)
#define SHIM(name) static void shim_##name(EngineCPU *cpu)

/* Host services. */
void host_log(const char *fmt, ...);
/* Once a frame: restore the engine thread's default FPCR if anything changed it. */
void host_fp_environment_check(void);
extern unsigned host_fp_environment_repairs;
/* Engine-thread time split (d3d9_render.inc), cumulative: passes, pass ns,
 * readback ns, readback calls, draws, vertex ns, submit ns, upload ns,
 * Sleep(n) calls, Sleep(n) ns, Sleep(0) calls, spare. */
void host_draw_profile_snapshot(uint64_t out[12]);
/* Diagnostic A/B control, called only on the engine thread between frames. */
void host_texture_crc_set_fast(int enabled);
int host_texture_crc_fast_enabled(void);
/* Engine-thread-only pass wall time; stays cumulative with HALO_DRAW_PROFILE. */
uint64_t host_pass_profile_total_ns(void);
/* The game's Sleep calls, counted in shims_kernel32.c. */
extern uint64_t host_sleep_calls, host_sleep_ns, host_yield_calls, host_yield_spin_ns;
/* ReadFileEx reads and bytes by file, cumulative (shims_kernel32.c): the
 * cache reader thread's reads of the level map, bitmaps.map and sounds.map,
 * then anything else. A sound missing from the sound cache is read here,
 * from sounds.map when it is not in the level map. */
enum { HOST_READ_LEVEL_MAP, HOST_READ_BITMAPS_MAP, HOST_READ_SOUNDS_MAP, HOST_READ_OTHER, HOST_READ_KINDS };
extern uint64_t host_async_reads[HOST_READ_KINDS], host_async_read_bytes[HOST_READ_KINDS];
/* The frame pacer's own blocking wait, with CLOCK_UPTIME_RAW endpoints:
 * counted once as presenting-thread idle, and the next Sleep(0) does not time
 * the gap across it as spin. Only the pacer calls this, only while pacing is
 * on; Sleep and SleepEx account exactly as they did before pacing existed. */
void host_frame_idle_wait(uint64_t start_ns, uint64_t end_ns);
void host_trace(const char *fmt, ...);            /* only when tracing is on */
extern int host_trace_imports;
extern const char *host_game_root;                 /* directory holding halo.exe and maps/ */
uint32_t guest_alloc(uint32_t size);               /* heap; 16-byte aligned; zeroed; fatal on exhaustion */
void guest_free(uint32_t ptr);                    /* exact live base only; invalid/duplicate frees ignored */
uint32_t guest_alloc_size(uint32_t ptr);           /* native requested size; zero for invalid/freed/interior */
/* Locked snapshot: requested live bytes, peak requested live bytes, cumulative
 * requested bytes, live allocation count, reserved native metadata bytes
 * (block slabs + static block/bin/hash storage), address high-water bytes
 * measured from heap start (includes allocation prefixes/alignment). */
void host_heap_stats(uint64_t out[6]);
/* Anonymous page space: live bytes, high-water offset, region size. */
void host_page_stats(uint64_t out[3]);
uint32_t guest_page_alloc(uint32_t size);          /* VirtualAlloc-style, 64 KiB granularity */
int guest_page_free(uint32_t base);               /* owning host resource only; exact allocation base */
uint32_t guest_strdup(const char *s);
uint32_t host_proc_address(const char *dll, const char *name);   /* magic callable for (dll,name) */
int host_proc_has_shim(uint32_t magic);                         /* callable is backed by a host implementation */
uint32_t host_module_handle(const char *dll);      /* magic HMODULE, or 0 if unknown */
const char *host_module_name(uint32_t handle);
extern _Thread_local uint32_t host_last_error;
void host_set_last_error(uint32_t e);
_Noreturn void host_exit(int code);
uint64_t host_filetime_now(void);                  /* 100 ns intervals since 1601 */
uint64_t host_monotonic_ns(void);
const char *host_path(const char *windows_path, char *buffer, size_t size); /* guest path -> host path */
uint32_t host_handle_new(int kind, void *object);   /* opaque HANDLE table */
void *host_handle_object(uint32_t handle, int kind);
void host_handle_close(uint32_t handle);
enum { HANDLE_FILE = 1, HANDLE_EVENT, HANDLE_MUTEX, HANDLE_THREAD, HANDLE_FIND, HANDLE_MAPPING, HANDLE_HEAP, HANDLE_MISC };
extern uint32_t host_main_hwnd;
void engine_dispatch(EngineCPU *cpu, uint32_t address);
int host_initialize_guest_thread(EngineCPU *cpu, uint32_t thread_id, uint32_t stack_size);
int host_execute_guest_thread(EngineCPU *cpu, uint32_t start_address,
                              uint32_t parameter, uint32_t *exit_code);
/* Call a guest function from a shim. callee_pops=1 for stdcall/PASCAL callbacks, 0 for cdecl. Returns eax. */
uint32_t host_call_guest(EngineCPU *cpu, uint32_t fn, int nargs, const uint32_t *args, int callee_pops);

/* Read an environment variable once and remember the answer.
 *
 * Every one of these is a diagnostic switch whose value is fixed for the life
 * of the process, and several sit inside the per-draw path, which runs
 * thousands of times a frame. getenv walks the whole environment on each
 * call, so a handful of switches in a draw becomes millions of string
 * comparisons a second for answers that never change. Each call site keeps
 * its own cache, and the value returned is exactly what getenv would have
 * given, so callers that compare strings or parse numbers are unaffected. */
#include <stdlib.h>
#define HOST_ENV(name) ({ static const char *host_env_value_; static int host_env_read_; \
    if (!host_env_read_) { host_env_read_ = 1; host_env_value_ = getenv(name); } host_env_value_; })
#endif
