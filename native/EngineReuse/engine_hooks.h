/* Guest function entries the host intercepts, and direct calls to the rest.
 *
 * engine_dispatch looks every call up: the override filter, the crash call
 * record, a binary search over 8336 entries and an indirect branch. On b30
 * that was about a tenth of the engine thread, and 25,835 of the 32,373
 * translated call sites name a fixed function entry. The generator emits
 * those as ENGINE_DIRECT, which calls the C function straight away unless
 * the entry is listed here. A listed entry still goes through
 * engine_dispatch, so engine_dispatch_override sees it. The test is on a
 * constant and folds away at each call site.
 *
 * Adding a hook means listing its address here and recompiling the
 * translated chunks; tests/test_dispatch_interest.py checks the lists cover
 * every address the hooks compare against and the override table.
 *
 * Direct calls skip engine_record, so the crash dump's call ring holds only
 * indirect calls; the native backtrace still shows the whole chain. */
#ifndef ENGINE_HOOKS_H
#define ENGINE_HOOKS_H
#include <stdint.h>

#define ENGINE_HOOK_ADDRESSES(X) \
    X(0x00577240u) /* native app updates; do not launch the Windows patcher */ \
    X(0x004C6E80u) /* campaign unlock and input/control tick */ \
    X(0x00442550u) X(0x00544090u) X(0x00544120u) X(0x0048A1A0u) /* audio/HSC */ \
    X(0x004D6FC0u) X(0x00533850u) X(0x00533730u) /* model capture */ \
    X(0x00492430u) X(0x0052B050u) X(0x00518F40u) X(0x004924B0u) /* panorama */ \
    X(0x005154A0u) X(0x005537C0u) X(0x0050CC40u) X(0x00449780u) \
    X(0x00494730u) X(0x004984C0u) X(0x0050BEA0u) X(0x0050BFB0u) X(0x0050BA80u) \
    X(0x0050F740u) /* object pixel size, one per object per panorama frame */ \
    X(0x0050BDC0u) /* panorama: the game's interface record */ \
    X(0x00626BA4u) X(0x00631930u) X(0x00631260u) /* overrides: _mbstowcs, _mbtowc, _wctomb */ \
    X(0x00634D8Eu) X(0x00634D50u) X(0x00634F61u) X(0x00634DCBu) /* FP intrinsic dispatchers */ \
    X(0x00449590u) /* int32 sort, native for the render pass's callback */ \
    X(0x00553920u) /* visible-triangle marking, native (visible_surfaces.h) */ \
    X(0x004CC0D0u) X(0x00554260u) X(0x005541B0u) X(0x00553380u) X(0x00552C20u) /* native leaves */ \
    X(0x00552DE0u) /* BSP material walk: the two with only a `ret` callback run natively */ \
    X(0x005540C0u) X(0x00553C40u) X(0x00553F10u) /* exact light/shadow gather loops */

/* HALO_A10_TRACE logging only. 00511F30 is a per-draw render call, so in
 * release these go direct; build the chunks with ENGINE_TRACE_HOOKS=1 to see
 * every call in the trace rather than only the indirect ones. 00552DE0, the
 * other one the trace logs, is a hook above now, so the trace sees every walk. */
#define ENGINE_TRACE_HOOK_ADDRESSES(X) \
    X(0x004C8800u) X(0x00477EA0u) X(0x005527F0u) X(0x005528F0u) X(0x00511F30u)

#ifndef ENGINE_TRACE_HOOKS
#define ENGINE_TRACE_HOOKS 0
#endif

static inline int engine_hooked(uint32_t address) {
    switch (address) {
#define ENGINE_HOOK_CASE(a) case a:
    ENGINE_HOOK_ADDRESSES(ENGINE_HOOK_CASE)
        return 1;
#if ENGINE_TRACE_HOOKS
    ENGINE_TRACE_HOOK_ADDRESSES(ENGINE_HOOK_CASE)
        return 1;
#endif
#undef ENGINE_HOOK_CASE
    default:
        return 0;
    }
}

/* Call the function at a fixed entry, as engine_dispatch would find it. */
#define ENGINE_DIRECT(cpu, address, fn) do { \
    if (engine_hooked(address)) engine_dispatch((cpu), (address)); \
    else { (cpu)->pc = (address); fn(cpu); } \
} while (0)

#endif
