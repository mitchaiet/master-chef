/* Host overrides for specific translated functions whose behavior is easier and safer to
   provide directly than to run through their locale-table-dependent original code.
   All are cdecl (caller cleans the stack), so RET_CDECL. */
#include "host.h"
#include "halo_settings.h"
#include "campaign_unlock.h"
#include "engine_hooks.h"
#include "multiplayer_update.h"

#include <math.h>
#include <string.h>
#define OARG(i) G32(cpu->gpr[4] + 4u + 4u * (uint32_t)(i))

/* MSVC FP intrinsic dispatchers (__cintrindisp1/2, __ctrandisp1/2): edx points to a descriptor
   whose first byte is a name length and the rest the op name ("fmod","exp","sinh",...). We read
   the operands off the x87 stack, compute with native math, and return, skipping __trandisp2. */
static void desc_name(EngineCPU *cpu, char *out) {
    uint32_t d = cpu->gpr[2]; uint32_t len = G8(d); if (len > 15) len = 15;
    for (uint32_t i = 0; i < len; i++) out[i] = (char)G8(d + 1 + i); out[len] = 0;
}
static double fp_op1(const char *n, double x) {
    if (!strcmp(n,"exp")) return exp(x);   if (!strcmp(n,"log")) return log(x);   if (!strcmp(n,"log10")) return log10(x);
    if (!strcmp(n,"sin")) return sin(x);   if (!strcmp(n,"cos")) return cos(x);   if (!strcmp(n,"tan")) return tan(x);
    if (!strcmp(n,"asin")) return asin(x); if (!strcmp(n,"acos")) return acos(x); if (!strcmp(n,"atan")) return atan(x);
    if (!strcmp(n,"sinh")) return sinh(x); if (!strcmp(n,"cosh")) return cosh(x); if (!strcmp(n,"tanh")) return tanh(x);
    if (!strcmp(n,"sqrt")) return sqrt(x); host_log("[fp] unknown 1-arg intrinsic '%s'", n); return x;
}
static double fp_op2(const char *n, double a, double b) {
    if (!strcmp(n,"pow")) return pow(a,b);   if (!strcmp(n,"fmod")) return fmod(a,b);   if (!strcmp(n,"atan2")) return atan2(a,b);
    if (!strcmp(n,"hypot")) return hypot(a,b); host_log("[fp] unknown 2-arg intrinsic '%s'", n); return a;
}
static void ret_to_caller(EngineCPU *cpu) { uint32_t ra = engine_pop(cpu, 4); cpu->pc = ra; }
static void ov_cintrindisp1(EngineCPU *cpu) { char n[16]; desc_name(cpu, n); double x = engine_fp_read(cpu, 0); engine_fp_write(cpu, 0, fp_op1(n, x)); ret_to_caller(cpu); }
static void ov_cintrindisp2(EngineCPU *cpu) { char n[16]; desc_name(cpu, n); double b = engine_fp_read(cpu, 0), a = engine_fp_read(cpu, 1); double r = fp_op2(n, a, b); engine_fp_pop(cpu); engine_fp_write(cpu, 0, r); ret_to_caller(cpu); }

/* size_t mbstowcs(wchar_t *dst, const char *src, size_t count) — C locale, one wchar per byte. */
static void ov_mbstowcs(EngineCPU *cpu) {
    uint32_t dst = OARG(0), src = OARG(1), count = OARG(2), n = 0;
    if (!src) RET_CDECL(0);
    for (;;) {
        uint8_t c = G8(src + n);
        if (dst) { if (n >= count) break; S16(dst + 2u * n, c); }
        if (c == 0) break;
        n++;
        if (dst && n >= count) break;
    }
    RET_CDECL(n);
}
/* size_t wcstombs(char *dst, const wchar_t *src, size_t count) */
static void ov_wcstombs(EngineCPU *cpu) {
    uint32_t dst = OARG(0), src = OARG(1), count = OARG(2), n = 0;
    if (!src) RET_CDECL(0);
    for (;;) {
        uint16_t w = G16(src + 2u * n);
        if (dst) { if (n >= count) break; S8(dst + n, (uint8_t)(w < 256 ? w : '?')); }
        if (w == 0) break;
        n++;
        if (dst && n >= count) break;
    }
    RET_CDECL(n);
}
/* int mbtowc(wchar_t *wc, const char *s, size_t n) — returns bytes consumed (0/1). */
static void ov_mbtowc(EngineCPU *cpu) {
    uint32_t wc = OARG(0), s = OARG(1), n = OARG(2);
    if (!s || !n) RET_CDECL(0);
    uint8_t c = G8(s);
    if (wc) S16(wc, c);
    RET_CDECL(c ? 1 : 0);
}
/* int wctomb(char *s, wchar_t wc) */
static void ov_wctomb(EngineCPU *cpu) {
    uint32_t s = OARG(0); uint16_t wc = (uint16_t)OARG(1);
    if (!s) RET_CDECL(0);
    S8(s, (uint8_t)(wc < 256 ? wc : '?'));
    RET_CDECL(1);
}

/* 00449590 sorts an array of int32 in place: eax = count, ecx = base,
 * [esp+4] = a "greater than" callback, caller cleans the stack. It is a
 * quicksort with an insertion sort (004496D0) for eight elements or fewer,
 * and every comparison is a translated call through the callback. The render
 * pass sorts with 00552C00, which is signed a > b, so the result is the
 * values in ascending signed order. Equal elements are equal values, so any
 * correct sort leaves the same bytes. Under 00552CF0 that sort was about a
 * tenth of each b30 bearing pass on the Mac. The other caller (00413D0F,
 * callback 004127B0) keeps the translated routine. */
static int int_sort_ascending(const void *a, const void *b) {
    int32_t x, y; memcpy(&x, a, 4); memcpy(&y, b, 4);
    return (x > y) - (x < y);
}
static int ov_int_sort(EngineCPU *cpu) {
    uint32_t count = cpu->gpr[0], base = cpu->gpr[1];
    if (G32(cpu->gpr[4] + 4u) != 0x00552C00u || count > 0x10000u) return 0;
    if (count >= 2) qsort(GPTR(base), count, 4, int_sort_ascending);
    uint32_t ra = engine_pop(cpu, 4);
    cpu->pc = ra;   /* eax and the flags are dead: every caller reloads them */
    return 1;
}

/* 00553920 marks the triangles of every subcluster in view (the frustum
 * test 0050D5B0 over each subcluster of each portal-visible cluster) and
 * runs natively; visible_surfaces.h has the details. HALO_NATIVE_VISIBILITY=0
 * keeps the translated routine. =verify runs the translated routine, then
 * the native one from the same starting state, compares what either writes
 * in the normal course (00553920's and 0050D5B0's stack frames, the
 * triangle bitset and its count) and the registers, flags and x87 state,
 * logs any difference, and keeps the translated result. */
#include "visible_surfaces.h"
#define VS_VERIFY_STACK 0x94u                          /* both frames, below the return address */
#define VS_VERIFY_BYTES (VS_MARKED + 2u - VS_BITSET)    /* the bitset and its count */
static int vs_verify_nested;
static uint64_t vs_verify_calls, vs_verify_mismatches;
static int vs_cpu_differs(const EngineCPU *a, const EngineCPU *b) {
    return memcmp(a->gpr, b->gpr, sizeof a->gpr) || a->flags != b->flags || a->pc != b->pc ||
           memcmp(a->fp_reg, b->fp_reg, sizeof a->fp_reg) || a->fp_valid != b->fp_valid || a->fp_top != b->fp_top ||
           a->fp_control != b->fp_control || a->fp_status != b->fp_status;
}
static int ov_visible_surfaces_verify(EngineCPU *cpu) {
    static uint8_t *before, *after;
    if (!before && !(before = malloc(2u * VS_VERIFY_BYTES))) return 0;
    after = before + VS_VERIFY_BYTES;
    uint8_t stack_before[VS_VERIFY_STACK], stack_after[VS_VERIFY_STACK];
    const uint32_t entry = cpu->gpr[4], stack = entry - VS_VERIFY_STACK, return_address = G32(entry);
    const EngineCPU start = *cpu;
    memcpy(before, GPTR(VS_BITSET), VS_VERIFY_BYTES); memcpy(stack_before, GPTR(stack), VS_VERIFY_STACK);
    vs_verify_nested = 1; engine_dispatch(cpu, 0x00553920u); vs_verify_nested = 0;
    if (cpu->pc != return_address) return 1;        /* left some other way: nothing to compare */
    const EngineCPU original = *cpu;
    memcpy(after, GPTR(VS_BITSET), VS_VERIFY_BYTES); memcpy(stack_after, GPTR(stack), VS_VERIFY_STACK);
    *cpu = start;
    memcpy(GPTR(VS_BITSET), before, VS_VERIFY_BYTES); memcpy(GPTR(stack), stack_before, VS_VERIFY_STACK);
    const int native = host_mark_visible_surfaces(cpu);
    const uint8_t *bits = GPTR(VS_BITSET), *frame = GPTR(stack);
    const int differs = native && (vs_cpu_differs(cpu, &original) || memcmp(bits, after, VS_VERIFY_BYTES) ||
                                   memcmp(frame, stack_after, VS_VERIFY_STACK));
    vs_verify_calls++;
    if (differs && vs_verify_mismatches++ < 16) {
        size_t at = 0;
        while (at < VS_VERIFY_BYTES && bits[at] == after[at]) at++;
        host_log("[visibility-native] MISMATCH call=%llu records=%d marked=%u/%u eax=%08x/%08x ecx=%08x/%08x edx=%08x/%08x "
                 "flags=%08x/%08x fpsw=%04x/%04x bitset@%ld stack=%d (native/translated)",
            (unsigned long long)vs_verify_calls, (int16_t)G16(VS_RECORD_COUNT),
            (unsigned)G16(VS_MARKED), (unsigned)(after[VS_MARKED - VS_BITSET] | after[VS_MARKED - VS_BITSET + 1u] << 8),
            cpu->gpr[0], original.gpr[0], cpu->gpr[1], original.gpr[1], cpu->gpr[2], original.gpr[2],
            cpu->flags, original.flags, cpu->fp_status, original.fp_status,
            at < VS_VERIFY_BYTES ? (long)at : -1L, memcmp(frame, stack_after, VS_VERIFY_STACK) != 0);
    }
    if (vs_verify_calls <= 4 || vs_verify_calls % 600 == 0)
        host_log("[visibility-native] verify: %llu calls, %llu mismatches%s", (unsigned long long)vs_verify_calls,
                 (unsigned long long)vs_verify_mismatches, native ? "" : " (the native routine declined the last)");
    *cpu = original;
    memcpy(GPTR(VS_BITSET), after, VS_VERIFY_BYTES); memcpy(GPTR(stack), stack_after, VS_VERIFY_STACK);
    return 1;
}
static int ov_visible_surfaces(EngineCPU *cpu) {
    static int mode = -1;   /* 0 translated, 1 native, 2 verify */
    if (mode < 0) {
        const char *o = HOST_ENV("HALO_NATIVE_VISIBILITY");
        mode = o && !strcmp(o, "0") ? 0 : o && !strcmp(o, "verify") ? 2 : 1;
        host_log("[visibility-native] 00553920 %s", mode == 0 ? "translated" : mode == 2 ? "verified against the translated routine" : "native");
    }
    if (mode == 0 || vs_verify_nested) return 0;
    if (mode == 2) return ov_visible_surfaces_verify(cpu);
    return host_mark_visible_surfaces(cpu);
}

typedef struct { uint32_t addr; HostShim fn; const char *name; } Override;
#include "a10_control.inc"
#include "a10_gamepad.inc"
#include "hsc_trace.inc"
#include "panorama_hooks.inc"
#include "frame_pacing_hooks.inc"
#include "model_capture_hooks.inc"
#include "audio_ownership_trace.inc"
#include "native_leaves.h"
#include "native_gather.h"
#include "native_gather_tree.h"
#include "gather_diagnostics.h"
#include <stdatomic.h>
static _Atomic uint64_t gather_native_count, gather_fallback_count;
static _Atomic uint32_t gather_enabled;
void host_gather_get_diagnostics(HostGatherDiagnostics *out) {
    if (!out) return;
    out->enabled = atomic_load_explicit(&gather_enabled, memory_order_relaxed);
    out->native_calls = atomic_load_explicit(&gather_native_count, memory_order_relaxed);
    out->fallback_calls = atomic_load_explicit(&gather_fallback_count, memory_order_relaxed);
    out->calls = out->native_calls + out->fallback_calls;
}
/* The Vision app opts in from Build82; desktop remains opt-in. Each entry
 * rereads geometry and this bearing's visibility bits; no cross-view cache. */
static int ov_native_gather(EngineCPU *cpu, uint32_t address) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = HOST_ENV("HALO_NATIVE_GATHER");
        enabled = value && !strcmp(value, "1");
        atomic_store_explicit(&gather_enabled, (uint32_t)enabled, memory_order_relaxed);
        host_log("[gather-native] HALO_NATIVE_GATHER=%d (%s)", enabled, enabled ? "exact native loops" : "translated; default");
    }
    if (!enabled) return 0;
    int taken = address == 0x005540C0u ? gather_native_leaf(cpu) :
                address == 0x00553C40u ? gather_native_clusters(cpu) : gather_native_tree(cpu);
    atomic_fetch_add_explicit(taken ? &gather_native_count : &gather_fallback_count, 1, memory_order_relaxed);
    return taken;
}
static Override overrides[] = {
    { 0x00577240u, host_multiplayer_update_poll, "native app update policy" },
    { 0x00626BA4u, ov_mbstowcs, "_mbstowcs" },
    { 0xFFFFFFFFu, ov_wcstombs, "_wcstombs (absent in this build)" },
    { 0x00631930u, ov_mbtowc,   "_mbtowc" },
    { 0x00631260u, ov_wctomb,   "_wctomb" },
    { 0x00634D8Eu, ov_cintrindisp1, "__cintrindisp1" },
    { 0x00634D50u, ov_cintrindisp2, "__cintrindisp2" },
    { 0x00634F61u, ov_cintrindisp1, "__ctrandisp1" },
    { 0x00634DCBu, ov_cintrindisp2, "__ctrandisp2" },
};
/* Conservative membership filter for every dispatch hook and override. Fold
 * addresses into 64K bits: collisions only take the unchanged slow path; a
 * missing bit can safely skip every probe. Initialized before guest threads
 * start, and read-only thereafter. Keep hook addresses covered by
 * tests/test_dispatch_interest.py when adding a boundary. */
static uint64_t dispatch_interest[1024];
/* The same lists decide which translated calls still come through here
 * (engine_hooks.h); the trace hooks see indirect calls unless the chunks
 * are built with ENGINE_TRACE_HOOKS=1. */
#define DISPATCH_HOOK_ADDRESS(a) a,
static const uint32_t dispatch_hook_addresses[] = {
    ENGINE_HOOK_ADDRESSES(DISPATCH_HOOK_ADDRESS)
    ENGINE_TRACE_HOOK_ADDRESSES(DISPATCH_HOOK_ADDRESS)
};
#undef DISPATCH_HOOK_ADDRESS
static void dispatch_interest_add(uint32_t address) {
    dispatch_interest[(address & 0xffffu) >> 6] |= UINT64_C(1) << (address & 63u);
}
__attribute__((constructor)) static void dispatch_interest_init(void) {
    for(size_t i=0;i<sizeof dispatch_hook_addresses/sizeof *dispatch_hook_addresses;i++)
        dispatch_interest_add(dispatch_hook_addresses[i]);
    for(size_t i=0;i<sizeof overrides/sizeof *overrides;i++)
        dispatch_interest_add(overrides[i].addr);
}
int engine_dispatch_override(EngineCPU *cpu, uint32_t address) {
    if (!(dispatch_interest[(address & 0xffffu) >> 6] & (UINT64_C(1) << (address & 63u)))) return 0;
    if (host_native_leaf_dispatch(cpu, address)) return 1;
    if ((address == 0x005540C0u || address == 0x00553C40u || address == 0x00553F10u) && ov_native_gather(cpu, address)) return 1;
    /* Apply after profile selection/replacement on the next main-loop tick.
     * In-memory unlock only: existing checkpoints and save files are untouched. */
    if (address == 0x004C6E80u) {
        const char *unlock = getenv("HALO_UNLOCK_CAMPAIGN");
        if (unlock && !strcmp(unlock, "1"))
            halo_unlock_campaign_menu((uint8_t *)GPTR(0x00712DD8u));
    }
    if (host_audio_ownership_dispatch(cpu, address)) return 1;
    if (host_model_capture_dispatch(cpu, address)) return 1;
    if (host_panorama_dispatch(cpu, address)) return 1;
    host_a10_gamepad_boundary(cpu, address);
    host_a10_boundary(cpu, address);
    host_hsc_trace_boundary(cpu, address);
    if (address == 0x00449590u && ov_int_sort(cpu)) return 1;
    if (address == 0x00553920u && ov_visible_surfaces(cpu)) return 1;
    if (address == 0x00552DE0u && host_native_leaves_enabled() && leaf_bsp_walk_dead(cpu)) return 1;
    for (size_t i = 0; i < sizeof overrides / sizeof overrides[0]; i++)
        if (overrides[i].addr == address) {
            static int announced[8];
            if (i < 8 && !announced[i]) { announced[i] = 1; host_trace("[override] %s at %08X", overrides[i].name, address); }
            overrides[i].fn(cpu); return 1;
        }
    return 0;
}
