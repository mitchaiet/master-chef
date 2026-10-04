/* Flat-memory host: loads the PE image, serves imports through native shims, runs the entry point. */
#include "host.h"
#include "directsound.h"
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include <signal.h>
#include <strings.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <dirent.h>

uint8_t *engine_flat_base;
/* Optional symbol table for backtraces: lines "ADDR name" from <game>/halo-vision-symbols.txt. */
static struct { uint32_t addr; char name[64]; } g_syms[16384]; static int g_symcount, g_symloaded;
static void load_syms(void) {
    if (g_symloaded) return; g_symloaded = 1; char path[1024]; snprintf(path, sizeof path, "%s/halo-vision-symbols.txt", host_game_root);
    FILE *f = fopen(path, "r"); if (!f) return; char line[256];
    while (fgets(line, sizeof line, f) && g_symcount < 16384) { unsigned a; char nm[64];
        if (sscanf(line, "%x %63s", &a, nm) == 2) { g_syms[g_symcount].addr = a; snprintf(g_syms[g_symcount].name, 64, "%s", nm); g_symcount++; } }
    fclose(f);
}
static const char *sym_for(uint32_t a) {
    load_syms(); const char *best = NULL; uint32_t bestaddr = 0;
    for (int i = 0; i < g_symcount; i++) if (g_syms[i].addr <= a && g_syms[i].addr >= bestaddr) { bestaddr = g_syms[i].addr; best = g_syms[i].name; }
    return best;
}
void host_backtrace(EngineCPU *cpu, const char *why) {
    host_log("---- guest backtrace (%s) pc=%08X ----", why, cpu->pc);
    const char *s = sym_for(cpu->pc); if (s) host_log("   pc  %08X  %s", cpu->pc, s);
    uint32_t esp = cpu->gpr[4]; int shown = 0;
    if (esp < 0x20000u || esp > 0xFFFF0000u) { host_log("   (esp %08X out of range; stack overflow?)", esp); host_log("----"); return; }
    for (uint32_t i = 0; i < 128 && shown < 20; i++) {
        uint32_t v = G32(esp + 4 * i);
        if (v >= 0x00401000u && v < 0x00639596u) { const char *nm = sym_for(v); host_log("   [esp+%-3u] %08X  %s", 4 * i, v, nm ? nm : "?"); shown++; }
    }
    host_log("----");
}
int host_trace_imports = 1;
const char *host_game_root = ".";
_Thread_local uint32_t host_last_error;
extern _Thread_local EngineCPU *host_active_cpu;
_Thread_local const char *host_current_import = "(none)";
static _Thread_local uint32_t call_ring[256]; static _Thread_local uint32_t call_ring_i;
void engine_record(uint32_t a){ call_ring[call_ring_i++ & 255] = a;}
static const char *sym_for(uint32_t a);
static void dump_call_ring(void){ host_log("---- last 48 dispatched functions ----");
  for (int k = 48; k >= 1; k--){ uint32_t a = call_ring[(call_ring_i - (uint32_t)k) & 255]; if(!a) continue; const char *n = sym_for(a); host_log("   %08X %s", a, n?n:""); } host_log("----"); }
uint32_t engine_trace_lo = 0xFFFFFFFFu, engine_trace_hi = 0;
#include "host_snapshot.h"
void engine_pc_trace(EngineCPU *cpu){ host_capture_for_test(cpu);host_log("TRACE %08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X flags=%08X", cpu->pc, cpu->gpr[0], cpu->gpr[3], cpu->gpr[1], cpu->gpr[2], cpu->gpr[6], cpu->gpr[7], cpu->gpr[5], cpu->gpr[4],cpu->flags); }
uint32_t host_main_hwnd = 0x00010001u;

#define GUEST_SIZE      UINT64_C(0x100000000)
#define STACK_BASE      0x00100000u
#define STACK_SIZE      0x00100000u
#define HEAP_START      0x02000000u
#define HEAP_END        0x40000000u
#define PAGE_START      0x50000000u
#define PAGE_END        0x7F000000u
#define MAIN_TEB_BASE   0x7FFDE000u
#define TEB_BASE        ((host_active_cpu && host_active_cpu->fs_base) ? host_active_cpu->fs_base : MAIN_TEB_BASE)
#define PEB_BASE        0x7FFDF000u
#define MAGIC_PROC      0xFF000000u
#define MAGIC_MODULE    0xFE000000u

void host_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fputs("[host] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
void host_trace(const char *fmt, ...) {
    if (!host_trace_imports) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
void host_set_last_error(uint32_t e) { host_last_error = e; S32(TEB_BASE + 0x34, e); }

/* ---------------- reusable guest heap ----------------
 * Preserve the original window and 16-byte prefix/alignment. The size dword
 * at payload-4 is a compatibility mirror only: ownership, sizes and links are
 * native metadata, never trusted guest bytes. Free ranges are size-binned and
 * linked in address order for splitting/coalescing. Exact live bases are hashed
 * separately, so interior/fabricated frees cannot damage the allocator.
 *
 * Native nodes come from reusable slabs. Coalescing recycles nodes; cumulative
 * allocate/free turnover therefore cannot grow metadata without a new peak in
 * simultaneously fragmented ranges. Aliases remain valid until their owner
 * frees the allocation; raw guest pointers cannot distinguish a stale alias
 * after that same address is allocated again. */
#define HEAP_HEADER_SIZE 16u
#define HEAP_MIN_SPAN 32u /* even a zero-byte allocation owns a distinct payload */
#define HEAP_BIN_COUNT 32u
#define HEAP_HASH_COUNT 16384u
#define HEAP_NODES_PER_SLAB 256u
typedef struct HeapBlock HeapBlock;
struct HeapBlock {
    uint32_t base, span, requested;
    int live;
    HeapBlock *prev, *next;           /* physical neighbors */
    HeapBlock *free_prev, *free_next; /* size bin */
    HeapBlock *hash_next;            /* live-base hash, or recycled-node chain */
};
typedef struct HeapSlab {
    struct HeapSlab *next;
    HeapBlock nodes[HEAP_NODES_PER_SLAB];
} HeapSlab;
static HeapBlock heap_initial = { .base = HEAP_START, .span = HEAP_END - HEAP_START };
static HeapBlock *heap_bins[HEAP_BIN_COUNT], *heap_hash[HEAP_HASH_COUNT], *heap_spare;
static HeapSlab *heap_slabs;
static uint32_t heap_slab_count;
static int heap_initialized;
static uint32_t heap_ptr = HEAP_START; /* address high-water, never an allocation cursor */
static uint64_t heap_live_bytes, heap_peak_bytes, heap_total_bytes;
static uint32_t heap_live_blocks;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned heap_bin(uint32_t span) {
    return 31u - (unsigned)__builtin_clz(span);
}
static unsigned heap_hash_bucket(uint32_t payload) {
    /* Fold high address bits into the bucket, including large aligned blocks. */
    uint32_t key = payload >> 4;
    key ^= key >> 16; key *= 0x7feb352du; key ^= key >> 15;
    return key & (HEAP_HASH_COUNT - 1u);
}
static void heap_bin_insert(HeapBlock *b) {
    unsigned bin = heap_bin(b->span);
    b->free_prev = NULL; b->free_next = heap_bins[bin];
    if (b->free_next) b->free_next->free_prev = b;
    heap_bins[bin] = b;
}
static void heap_bin_remove(HeapBlock *b) {
    if (b->free_prev) b->free_prev->free_next = b->free_next;
    else heap_bins[heap_bin(b->span)] = b->free_next;
    if (b->free_next) b->free_next->free_prev = b->free_prev;
    b->free_prev = b->free_next = NULL;
}
static HeapBlock *heap_node_acquire(void) {
    if (!heap_spare) {
        HeapSlab *slab = calloc(1, sizeof *slab);
        if (!slab) return NULL;
        slab->next = heap_slabs; heap_slabs = slab; heap_slab_count++;
        for (unsigned i = 0; i < HEAP_NODES_PER_SLAB; i++) {
            slab->nodes[i].hash_next = heap_spare; heap_spare = &slab->nodes[i];
        }
    }
    HeapBlock *b = heap_spare;
    heap_spare = b->hash_next;
    memset(b, 0, sizeof *b);
    return b;
}
static void heap_node_recycle(HeapBlock *b) {
    b->hash_next = heap_spare; heap_spare = b;
}
static HeapBlock **heap_find_live(uint32_t ptr) {
    HeapBlock **link = &heap_hash[heap_hash_bucket(ptr)];
    while (*link && (*link)->base + HEAP_HEADER_SIZE != ptr) link = &(*link)->hash_next;
    return link;
}
static _Noreturn void heap_fail(const char *reason, uint32_t size) {
    /* Never escape while holding heap_lock: main-context failures longjmp. */
    host_log("guest heap %s at %08X (+%u), import=%s", reason,
             __atomic_load_n(&heap_ptr, __ATOMIC_RELAXED), size, host_current_import);
    if (host_active_cpu) host_backtrace(host_active_cpu, "guest heap allocation failure");
    host_exit(3);
}
uint32_t guest_alloc(uint32_t size) {
    /* Keep the established single-allocation ceiling, with wide rounding. */
    if (size > 0x20000000u) heap_fail("request too large", size);
    uint64_t rounded = ((uint64_t)size + 15u) & ~UINT64_C(15);
    if (!rounded) rounded = 16u;
    uint32_t span = (uint32_t)(rounded + HEAP_HEADER_SIZE);
    pthread_mutex_lock(&heap_lock);
    if (!heap_initialized) { heap_bin_insert(&heap_initial); heap_initialized = 1; }
    HeapBlock *b = NULL;
    for (unsigned bin = heap_bin(span); bin < HEAP_BIN_COUNT && !b; bin++)
        for (HeapBlock *candidate = heap_bins[bin]; candidate; candidate = candidate->free_next)
            if (candidate->span >= span) { b = candidate; break; }
    if (!b) {
        pthread_mutex_unlock(&heap_lock);
        heap_fail("exhausted", size);
    }
    HeapBlock *remainder = NULL;
    if (b->span - span >= HEAP_MIN_SPAN) {
        remainder = heap_node_acquire();
        if (!remainder) {
            pthread_mutex_unlock(&heap_lock);
            heap_fail("metadata exhausted", size);
        }
    }
    heap_bin_remove(b);
    if (remainder) {
        remainder->base = b->base + span; remainder->span = b->span - span;
        remainder->prev = b; remainder->next = b->next;
        if (remainder->next) remainder->next->prev = remainder;
        b->next = remainder; b->span = span;
        heap_bin_insert(remainder);
    }
    b->live = 1; b->requested = size;
    uint32_t payload = b->base + HEAP_HEADER_SIZE;
    unsigned bucket = heap_hash_bucket(payload);
    b->hash_next = heap_hash[bucket]; heap_hash[bucket] = b;
    heap_live_blocks++; heap_live_bytes += size; heap_total_bytes += size;
    if (heap_live_bytes > heap_peak_bytes) heap_peak_bytes = heap_live_bytes;
    uint32_t end = b->base + b->span;
    if (end > __atomic_load_n(&heap_ptr, __ATOMIC_RELAXED))
        __atomic_store_n(&heap_ptr, end, __ATOMIC_RELAXED);
    /* Clear the prefix and all rounded payload/padding on every allocation,
     * including recycled bytes and tiny tails that cannot be split. */
    memset(GPTR(b->base), 0, b->span);
    S32(payload - 4u, size);
    pthread_mutex_unlock(&heap_lock);
    return payload;
}
uint32_t guest_alloc_size(uint32_t ptr) {
    if (ptr < HEAP_START + HEAP_HEADER_SIZE || ptr >= HEAP_END || (ptr & 15u)) return 0;
    pthread_mutex_lock(&heap_lock);
    HeapBlock *b = *heap_find_live(ptr);
    uint32_t size = b ? b->requested : 0;
    pthread_mutex_unlock(&heap_lock);
    return size;
}
void guest_free(uint32_t ptr) {
    if (ptr < HEAP_START + HEAP_HEADER_SIZE || ptr >= HEAP_END || (ptr & 15u)) return;
    pthread_mutex_lock(&heap_lock);
    HeapBlock **link = heap_find_live(ptr), *b = *link;
    if (!b) { pthread_mutex_unlock(&heap_lock); return; }
    *link = b->hash_next; b->hash_next = NULL;
    heap_live_blocks--; heap_live_bytes -= b->requested;
    b->live = 0; b->requested = 0;
    if (b->prev && !b->prev->live) {
        HeapBlock *prev = b->prev;
        heap_bin_remove(prev);
        prev->span += b->span; prev->next = b->next;
        if (b->next) b->next->prev = prev;
        heap_node_recycle(b); b = prev;
    }
    if (b->next && !b->next->live) {
        HeapBlock *next = b->next;
        heap_bin_remove(next);
        b->span += next->span; b->next = next->next;
        if (next->next) next->next->prev = b;
        heap_node_recycle(next);
    }
    heap_bin_insert(b);
    pthread_mutex_unlock(&heap_lock);
}
void host_heap_stats(uint64_t out[6]) {
    if (!out) return;
    pthread_mutex_lock(&heap_lock);
    out[0] = heap_live_bytes;
    out[1] = heap_peak_bytes;
    out[2] = heap_total_bytes;
    out[3] = heap_live_blocks;
    out[4] = sizeof heap_initial + sizeof heap_bins + sizeof heap_hash +
             (uint64_t)heap_slab_count * sizeof(HeapSlab);
    out[5] = __atomic_load_n(&heap_ptr, __ATOMIC_RELAXED) - HEAP_START;
    pthread_mutex_unlock(&heap_lock);
}
/* Page allocations are reclaimed only by their owning host resource. Keep the
 * allocation map outside guest memory, so a guest write cannot corrupt it.
 * A bitmap search naturally joins adjacent freed allocations without metadata
 * allocation or an ever-growing free list. Explicit game pool reservations
 * are excluded from this allocator and cannot be freed through this API. */
#define PAGE_GRANULARITY 0x10000u
#define PAGE_COUNT ((PAGE_END - PAGE_START) / PAGE_GRANULARITY)
#define PAGE_CONTINUATION UINT32_MAX
#define PAGE_EXPLICIT (UINT32_MAX - 1u)
static uint32_t page_allocations[PAGE_COUNT];
static uint8_t page_used[PAGE_COUNT];
static uint32_t page_top = PAGE_START; /* high-water diagnostic, not an allocation cursor */
static uint32_t page_live_granules, page_logged_step; /* live anonymous granules; last 32 MiB step logged */
static pthread_mutex_t page_lock = PTHREAD_MUTEX_INITIALIZER;
/* Page space is finite (752 MiB) and only owners give pages back, so log each
 * 32 MiB step of live use in either direction: a leak across level loads shows
 * as a staircase, reclamation as a rise and fall. */
static void page_note_growth_locked(void) {
    uint32_t step = (uint32_t)(((uint64_t)page_live_granules * PAGE_GRANULARITY) >> 25);
    if (step != page_logged_step) {
        page_logged_step = step;
        host_log("[pages] live=%u MiB high-water=%u MiB of %u MiB",
                 (uint32_t)(((uint64_t)page_live_granules * PAGE_GRANULARITY) >> 20),
                 (page_top - PAGE_START) >> 20, (PAGE_END - PAGE_START) >> 20);
    }
}
void host_page_stats(uint64_t out[3]) {
    if (!out) return;
    pthread_mutex_lock(&page_lock);
    out[0] = (uint64_t)page_live_granules * PAGE_GRANULARITY;
    out[1] = page_top - PAGE_START;
    out[2] = PAGE_END - PAGE_START;
    pthread_mutex_unlock(&page_lock);
}
uint32_t guest_page_alloc(uint32_t size) {
    if (!size) return 0;
    uint64_t rounded = ((uint64_t)size + PAGE_GRANULARITY - 1u) & ~(uint64_t)(PAGE_GRANULARITY - 1u);
    if (rounded > PAGE_END - PAGE_START) {
        host_log("guest page allocation too large (%u bytes)", size);
        host_exit(3);
    }
    uint32_t count = (uint32_t)(rounded / PAGE_GRANULARITY);
    pthread_mutex_lock(&page_lock);
    uint32_t run = 0, first = 0;
    for (uint32_t i = 0; i < PAGE_COUNT; i++) {
        if (page_allocations[i]) run = 0;
        else if (++run == count) { first = i + 1u - count; break; }
    }
    if (run != count) {
        pthread_mutex_unlock(&page_lock);
        host_log("guest page space exhausted (+%u bytes)", size);
        host_exit(3);
    }
    page_allocations[first] = count;
    for (uint32_t i = 1; i < count; i++) page_allocations[first + i] = PAGE_CONTINUATION;
    uint32_t a = PAGE_START + first * PAGE_GRANULARITY;
    if ((uint64_t)a + rounded > page_top) page_top = a + (uint32_t)rounded;
    page_live_granules += count;
    page_note_growth_locked();
    /* Initial mmap pages are zero; recycled pages must provide that same
     * contract, including alignment padding visible through guest pointers.
     * Keep untouched mmap pages lazy, as they were in the bump allocator. */
    for (uint32_t i = 0; i < count; i++) {
        if (page_used[first + i]) memset(GPTR(a + i * PAGE_GRANULARITY), 0, PAGE_GRANULARITY);
        page_used[first + i] = 1;
    }
    pthread_mutex_unlock(&page_lock);
    return a;
}
int guest_page_free(uint32_t addr) {
    if (addr < PAGE_START || addr >= PAGE_END || (addr & (PAGE_GRANULARITY - 1u))) return 0;
    uint32_t first = (addr - PAGE_START) / PAGE_GRANULARITY;
    pthread_mutex_lock(&page_lock);
    uint32_t count = page_allocations[first];
    if (!count || count > PAGE_COUNT - first) {
        pthread_mutex_unlock(&page_lock);
        return 0;
    }
    memset(page_allocations + first, 0, count * sizeof *page_allocations);
    page_live_granules -= count < page_live_granules ? count : page_live_granules;
    page_note_growth_locked();
    /* Hand the freed granules back as fresh zero pages. Reuse then stays lazy,
     * instead of the clearing memset making a whole 64 KiB granule resident for
     * a small mip level. If the remap fails, the clearing path still applies. */
    if (mmap(GPTR(addr), (size_t)count * PAGE_GRANULARITY, PROT_READ | PROT_WRITE,
             MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != MAP_FAILED)
        memset(page_used + first, 0, count);
    pthread_mutex_unlock(&page_lock);
    return 1;
}
/* Explicit-address reservations (the game hardcodes 0x40000000 for its pools):
 * honor them without making any intersecting anonymous pages reusable. Calls
 * committing an existing anonymous reservation leave its ownership intact. */
uint32_t guest_page_reserve_explicit(uint32_t addr, uint32_t size) {
    uint64_t end = ((uint64_t)addr + size + PAGE_GRANULARITY - 1u) & ~(uint64_t)(PAGE_GRANULARITY - 1u);
    if (!size || addr < HEAP_END || addr >= PAGE_END || end > PAGE_END) return 0;
    pthread_mutex_lock(&page_lock);
    if (end > PAGE_START) {
        uint32_t first = addr > PAGE_START ? (addr - PAGE_START) / PAGE_GRANULARITY : 0;
        uint32_t limit = (uint32_t)(end - PAGE_START) / PAGE_GRANULARITY;
        /* Existing anonymous reservations may be committed again, including
         * from an interior address. A new pool must not straddle or subsume
         * independent live host allocations. */
        for (uint32_t i = first; i < limit; i++) {
            if (!page_allocations[i] || page_allocations[i] == PAGE_EXPLICIT) continue;
            uint32_t owner = i;
            while (owner && page_allocations[owner] == PAGE_CONTINUATION) owner--;
            uint64_t owner_base = PAGE_START + owner * PAGE_GRANULARITY;
            uint64_t owner_end = owner_base + (uint64_t)page_allocations[owner] * PAGE_GRANULARITY;
            if (addr >= owner_base && end <= owner_end) {
                pthread_mutex_unlock(&page_lock);
                return addr;
            }
            pthread_mutex_unlock(&page_lock);
            return 0;
        }
        for (uint32_t i = first; i < limit; i++) {
            if (page_allocations[i]) continue;
            if (page_used[i]) memset(GPTR(PAGE_START + i * PAGE_GRANULARITY), 0, PAGE_GRANULARITY);
            page_used[i] = 1;
            page_allocations[i] = PAGE_EXPLICIT;
        }
    }
    if (end > page_top) page_top = (uint32_t)end;
    pthread_mutex_unlock(&page_lock);
    return addr;
}
uint32_t guest_strdup(const char *s) { uint32_t a = guest_alloc((uint32_t)strlen(s) + 1); strcpy((char *)GPTR(a), s); return a; }

/* ---------------- handles ---------------- */
typedef struct { int kind; void *object; } HostHandle;
static HostHandle handles[4096];
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t host_handle_new(int kind, void *object) {
    pthread_mutex_lock(&handles_lock);
    for (uint32_t i = 16; i < 4096; i++) if (!handles[i].kind) { handles[i].kind = kind; handles[i].object = object; pthread_mutex_unlock(&handles_lock); return i * 4u; }
    pthread_mutex_unlock(&handles_lock);
    host_log("handle table full"); host_exit(3);
}
void *host_handle_object(uint32_t h, int kind) { uint32_t i = h / 4u; pthread_mutex_lock(&handles_lock); void *object = (h % 4u == 0 && i < 4096 && handles[i].kind == kind) ? handles[i].object : NULL; pthread_mutex_unlock(&handles_lock); return object; }
void host_handle_close(uint32_t h) { uint32_t i = h / 4u; pthread_mutex_lock(&handles_lock); if (h % 4u == 0 && i < 4096) { handles[i].kind = 0; handles[i].object = NULL; } pthread_mutex_unlock(&handles_lock); }

/* ---------------- time ---------------- */
uint64_t host_monotonic_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); }
/* When the guest rounds to nearest, translated x87 arithmetic runs in the
 * host's own rounding mode (engine_cpu.h). That assumes the engine thread's
 * FPCR stays at its default: nearest, no flush-to-zero, no default NaN.
 * Nothing the host runs changes it; check once a frame, and put it back,
 * with one log line, if something ever does. */
unsigned host_fp_environment_repairs;
void host_fp_environment_check(void) {
#if defined(__aarch64__)
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    const uint64_t changed = (3ull << 22) | (1ull << 24) | (1ull << 25) | (1ull << 19) | 7ull; /* RMode, FZ, DN, FZ16, FIZ/AH/NEP */
    if (fpcr & changed) {
        if (!host_fp_environment_repairs++)
            host_log("[fp] engine thread FPCR %#llx is not the default; restoring it", (unsigned long long)fpcr);
        fpcr &= ~changed; __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
    }
#endif
}
uint64_t host_filetime_now(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (uint64_t)ts.tv_sec * 10000000u + (uint64_t)ts.tv_nsec / 100u + UINT64_C(116444736000000000); }

/* ---------------- paths ---------------- */
#include "host_path.inc"

/* ---------------- imports / magic procs ---------------- */
typedef struct { char dll[32]; char name[64]; HostShim fn; uint32_t calls; } Proc;
static Proc procs[8192]; static uint32_t proc_count = 1; /* index 0 = return-to-host */
static pthread_mutex_t procs_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *known_modules[] = { "KERNEL32.dll","USER32.dll","ADVAPI32.dll","GDI32.dll","ole32.dll","OLEAUT32.dll",
    "WSOCK32.dll","WS2_32.dll","BINKW32.dll","VORBISFILE.dll","VERSION.dll","WINMM.dll","WININET.dll","SHELL32.dll","DSOUND.dll",
    "d3d9.dll","dinput8.dll","keystone.dll","strings.dll","shfolder.dll","eula.dll","ddraw.dll","psapi.dll","winhttp.dll","faultrep.dll","NVCPL.dll","mscoree.dll", NULL };
static int dll_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la >= 4 && !strcasecmp(a + la - 4, ".dll")) la -= 4;
    if (lb >= 4 && !strcasecmp(b + lb - 4, ".dll")) lb -= 4;
    return la == lb && !strncasecmp(a, b, la);
}
const HostShimEntry *host_shims_d3d9_build(void); const HostShimEntry *host_shims_dinput8_build(void); const HostShimEntry *host_shims_ddraw_build(void);
static HostShim find_shim(const char *dll, const char *name) {
    static const HostShimEntry *d3d9_table, *dinput_table, *ddraw_table; if (!d3d9_table) { d3d9_table = host_shims_d3d9_build(); dinput_table = host_shims_dinput8_build(); ddraw_table = host_shims_ddraw_build(); }
    const HostShimEntry *tables[] = { host_shims_kernel32, host_shims_misc, host_shims_winsock, d3d9_table, dinput_table, ddraw_table };
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (const HostShimEntry *e = tables[t]; e->name; e++)
            if (dll_equal(e->dll, dll) && !strcmp(e->name, name)) return e->fn;
    return NULL;
}
uint32_t host_proc_address(const char *dll, const char *name) {
    pthread_mutex_lock(&procs_lock);
    for (uint32_t i = 1; i < proc_count; i++) if (dll_equal(procs[i].dll, dll) && !strcmp(procs[i].name, name)) { pthread_mutex_unlock(&procs_lock); return MAGIC_PROC | i; }
    if (proc_count >= 8192) { pthread_mutex_unlock(&procs_lock); host_log("proc table full"); host_exit(3); }
    Proc *p = &procs[proc_count];
    snprintf(p->dll, sizeof p->dll, "%s", dll); snprintf(p->name, sizeof p->name, "%s", name);
    p->fn = find_shim(dll, name);
    uint32_t result = MAGIC_PROC | proc_count++;
    pthread_mutex_unlock(&procs_lock);
    return result;
}
uint32_t host_module_handle(const char *dll) {
    for (uint32_t i = 0; known_modules[i]; i++) if (dll_equal(known_modules[i], dll)) return MAGIC_MODULE | (i + 1);
    return 0;
}
const char *host_module_name(uint32_t handle) {
    if ((handle & 0xFF000000u) != MAGIC_MODULE) return NULL;
    uint32_t i = (handle & 0xFFFFFFu); if (!i) return NULL;
    for (uint32_t k = 0; known_modules[k]; k++) if (k + 1 == i) return known_modules[k];
    return NULL;
}
static _Thread_local jmp_buf host_escape;
static _Thread_local const char *host_escape_reason;
static _Thread_local int host_exit_code;
static _Thread_local int host_escape_ready;
static _Thread_local int host_is_main_context;
_Noreturn void host_exit(int code) {
    if (!host_is_main_context) _exit(code);
    host_exit_code = code;
    host_escape_reason = "process exit";
    if (host_escape_ready) longjmp(host_escape, 2);
    _exit(code);
}

uint32_t host_call_guest(EngineCPU *cpu, uint32_t fn, int nargs, const uint32_t *args, int callee_pops) {
    uint32_t saved_esp = cpu->gpr[4], saved_pc = cpu->pc;
    for (int i = nargs - 1; i >= 0; i--) engine_push(cpu, args[i], 4);
    engine_push(cpu, MAGIC_PROC | 0u, 4);
    engine_dispatch(cpu, fn);
    if (cpu->pc != (MAGIC_PROC | 0u)) { host_log("guest callback %08X returned to %08X, not to the host", fn, cpu->pc); engine_fail(cpu, "callback return mismatch"); }
    if (!callee_pops) cpu->gpr[4] += 4u * (uint32_t)nargs;
    if (cpu->gpr[4] != saved_esp) { host_log("guest callback %08X left the stack unbalanced (%08X vs %08X)", fn, cpu->gpr[4], saved_esp); cpu->gpr[4] = saved_esp; }
    cpu->pc = saved_pc;
    return cpu->gpr[0];
}
int engine_dispatch_external(EngineCPU *cpu, uint32_t address) {
    if ((address & 0xFF000000u) != MAGIC_PROC) return 0;
    uint32_t i = address & 0xFFFFFFu;
    if (i == 0) { host_escape_reason = "entry point returned"; longjmp(host_escape, 3); }
    pthread_mutex_lock(&procs_lock);
    if (i >= proc_count) { pthread_mutex_unlock(&procs_lock); return 0; }
    Proc *p = &procs[i];
    uint32_t calls = __atomic_add_fetch(&p->calls, 1u, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&procs_lock);
    if (host_trace_imports && (calls <= 8 || (calls & (calls - 1)) == 0))
        host_trace("[import] %s!%s (call %u) args=%08X %08X %08X %08X", p->dll, p->name, calls, ARG(0), ARG(1), ARG(2), ARG(3));
    host_current_import = p->name;
    if (!p->fn) { host_log("UNIMPLEMENTED import %s!%s  (return address %08X)", p->dll, p->name, G32(cpu->gpr[4])); cpu->pc = address; engine_fail(cpu, "unimplemented import"); }
    p->fn(cpu);
    return 1;
}

/* ---------------- PE loading ---------------- */
static void load_image(const char *exe) {
    int fd = open(exe, O_RDONLY); if (fd < 0) { host_log("cannot open %s", exe); exit(2); }
    struct stat st; fstat(fd, &st);
    uint8_t *file = malloc((size_t)st.st_size); read(fd, file, (size_t)st.st_size); close(fd);
    memcpy(GPTR(engine_pe_image_base), file, 0x1000);  /* headers */
    for (size_t i = 0; i < engine_pe_section_count; i++) {
        const EngineSection *s = &engine_pe_sections[i];
        uint32_t n = s->raw_size < s->virtual_size ? s->raw_size : s->virtual_size;
        if (s->raw_offset + n <= (uint32_t)st.st_size) memcpy(GPTR(s->va), file + s->raw_offset, n);
        host_trace("[load] %-8s %08X size %08X", s->name, s->va, s->virtual_size);
    }
    free(file);
}
static void setup_imports(void) {
    for (size_t i = 0; i < engine_import_count; i++) {
        const EngineImport *imp = &engine_imports[i];
        S32(imp->iat_va, host_proc_address(imp->dll, imp->name));
    }
    unsigned missing = 0;
    for (uint32_t i = 1; i < proc_count; i++) if (!procs[i].fn) missing++;
    host_log("%zu static imports bound, %u without a shim yet", engine_import_count, missing);
}
static void setup_thread_teb(uint32_t teb, uint32_t stack_base,
                             uint32_t stack_size, uint32_t thread_id) {
    memset(GPTR(teb), 0, 0x1000);
    S32(teb + 0x00, 0xFFFFFFFFu);                     /* SEH chain end */
    S32(teb + 0x04, stack_base + stack_size);         /* stack base (high) */
    S32(teb + 0x08, stack_base);                      /* stack limit (low) */
    S32(teb + 0x18, teb);                             /* self */
    S32(teb + 0x20, 4660);                            /* pid */
    S32(teb + 0x24, thread_id);                       /* tid */
    S32(teb + 0x30, PEB_BASE);
    S32(PEB_BASE + 0x08, engine_pe_image_base);
    uint32_t tls_array = guest_alloc(64 * 4);
    S32(teb + 0x2C, tls_array);
    if (engine_pe_tls_directory) {
        uint32_t start = G32(engine_pe_tls_directory), end = G32(engine_pe_tls_directory + 4);
        uint32_t index_addr = G32(engine_pe_tls_directory + 8), zero = G32(engine_pe_tls_directory + 16);
        uint32_t block = guest_alloc(end - start + zero);
        memcpy(GPTR(block), GPTR(start), end - start);
        S32(index_addr, 0); S32(tls_array, block);
        host_trace("[load] thread %u TEB %08X TLS block %08X (%u+%u bytes)",
                   thread_id, teb, block, end - start, zero);
    }
}
static EngineCPU host_cpu; _Thread_local EngineCPU *host_active_cpu;
static void cpu_failure(EngineCPU *cpu, const char *reason) { (void)cpu; host_escape_reason = reason; longjmp(host_escape, 1); }

int host_initialize_guest_thread(EngineCPU *cpu, uint32_t thread_id, uint32_t stack_size) {
    if (!stack_size) stack_size = STACK_SIZE;
    if (stack_size < 0x10000u) stack_size = 0x10000u;
    stack_size = (stack_size + 0xFFFFu) & ~0xFFFFu;
    uint32_t stack_base = guest_page_alloc(stack_size);
    uint32_t teb = guest_page_alloc(0x1000);
    memset(cpu, 0, sizeof *cpu);
    cpu->failure = cpu_failure;
    cpu->fs_base = teb;
    cpu->gpr[4] = stack_base + stack_size - 0x100u;
    cpu->flags = 0x202u;
    engine_fp_init(cpu);
    setup_thread_teb(teb, stack_base, stack_size, thread_id);
    return 1;
}

int host_execute_guest_thread(EngineCPU *cpu, uint32_t start_address,
                              uint32_t parameter, uint32_t *exit_code) {
    host_active_cpu = cpu;
    host_is_main_context = 0;
    host_escape_ready = 1;
    host_escape_reason = NULL;
    engine_push(cpu, parameter, 4);
    engine_push(cpu, MAGIC_PROC | 0u, 4);
    int how = setjmp(host_escape);
    if (how == 0) {
        engine_dispatch(cpu, start_address);
        if (cpu->pc != (MAGIC_PROC | 0u))
            cpu_failure(cpu, "thread unwound to unresolved guest return");
        if (exit_code) *exit_code = cpu->gpr[0];
        host_escape_ready = 0;
        return 1;
    }
    host_log("guest thread stopped at %08X: %s", cpu->pc,
             host_escape_reason ? host_escape_reason : "unknown failure");
    host_backtrace(cpu, host_escape_reason ? host_escape_reason : "thread failure");
    if (exit_code) *exit_code = (uint32_t)(how == 2 ? host_exit_code : 1);
    host_escape_ready = 0;
    return 0;
}
static void heartbeat(int sig) { (void)sig;
    host_log("... alive: pc=%08X instr=%llu heap=%08X import=%s", host_cpu.pc, (unsigned long long)host_cpu.instruction_count, __atomic_load_n(&heap_ptr, __ATOMIC_RELAXED), host_current_import);
    alarm(15);
}
static volatile int in_segv;
static void segv(int sig) {
    if (in_segv) _exit(5); in_segv = 1;
    EngineCPU *fault_cpu = host_active_cpu ? host_active_cpu : &host_cpu;
    host_log("fatal signal %d at guest pc %08X (esp %08X)%s", sig, fault_cpu->pc, fault_cpu->gpr[4], fault_cpu->gpr[4] < 0x100000u ? "  <- GUEST STACK OVERFLOW" : "");
    { uint32_t esi = fault_cpu->gpr[6];
      if (esi >= 0x10000u && esi < 0xFFFF0000u) { uint32_t vt = G32(esi);
        host_log("   object=%08X vtable=%08X vt[0]=%08X vt[2]=%08X (call target) object[0x84]=%08X",
                 esi, vt, (vt>=0x10000u&&vt<0xFFFF0000u)?G32(vt):0, (vt>=0x10000u&&vt<0xFFFF0000u)?G32(vt+8):0, G32(esi+0x84)); } }
    void host_backtrace(EngineCPU*,const char*); host_backtrace(fault_cpu, "segv");
    _exit(4);
}
static void dump_cpu(EngineCPU *c) {
    host_log("eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X eflags=%08X pc=%08X",
             c->gpr[0], c->gpr[1], c->gpr[2], c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], c->gpr[7], c->flags, c->pc);
    host_log("stack: %08X %08X %08X %08X %08X %08X", G32(c->gpr[4]), G32(c->gpr[4] + 4), G32(c->gpr[4] + 8), G32(c->gpr[4] + 12), G32(c->gpr[4] + 16), G32(c->gpr[4] + 20));
    host_log("instructions executed: %llu", (unsigned long long)c->instruction_count);
}
void engine_reuse_entry(EngineCPU *cpu, uint32_t entry);

int host_run(const char *exe, const char *root) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    { const char *tl=getenv("HALO_TRACE_LO"), *th=getenv("HALO_TRACE_HI"); if(tl&&th){ engine_trace_lo=(uint32_t)strtoul(tl,0,16); engine_trace_hi=(uint32_t)strtoul(th,0,16); host_log("PC trace enabled %08X..%08X", engine_trace_lo, engine_trace_hi);} }
    host_game_root = root;
    engine_flat_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (engine_flat_base == MAP_FAILED) { host_log("cannot reserve guest address space"); return 2; }
    mprotect(engine_flat_base, 0x10000, PROT_NONE);   /* catch null dereferences */
    { static char altstk[262144]; stack_t ss = { .ss_sp = altstk, .ss_size = sizeof altstk, .ss_flags = 0 }; sigaltstack(&ss, NULL);
      struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = segv; sa.sa_flags = SA_ONSTACK; sigemptyset(&sa.sa_mask);
      sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); }
    signal(SIGALRM, heartbeat); alarm(15);
    load_image(exe);
    setup_imports();
    EngineCPU *cpu = &host_cpu; memset(cpu, 0, sizeof *cpu); host_active_cpu = cpu;
    host_is_main_context = 1;
    cpu->fs_base = MAIN_TEB_BASE;
    cpu->failure = cpu_failure; cpu->instruction_limit = 0;
    engine_fp_init(cpu);
    cpu->gpr[4] = STACK_BASE + STACK_SIZE - 0x100;
    setup_thread_teb(MAIN_TEB_BASE, STACK_BASE, STACK_SIZE, 1);
    engine_push(cpu, MAGIC_PROC | 0, 4);              /* return to host */
    cpu->flags = 0x202;
    host_log("starting at entry point %08X", engine_pe_entry_point);
    host_escape_ready = 1;
    int how = setjmp(host_escape);
    if (how == 0) {
        engine_reuse_entry(cpu, engine_pe_entry_point);
        if(cpu->pc != MAGIC_PROC) cpu_failure(cpu,"unwound to unresolved guest return");
        host_escape_reason = "entry returned normally";
    }
    if (how != 2) dump_call_ring();
    host_escape_ready = 0;
    host_log("stopped: %s", host_escape_reason);
    dump_cpu(cpu);
    host_winsock_shutdown();
    host_dsound_shutdown();
    uint64_t audio_frames=0,audio_nonzero=0;float audio_peak=0;
    host_dsound_get_stats(&audio_frames,&audio_nonzero,&audio_peak);
    host_log("[audio-result] frames=%llu nonzero=%llu peak=%.5f",
             (unsigned long long)audio_frames,(unsigned long long)audio_nonzero,audio_peak);
    return how == 2 ? host_exit_code : (how == 1 ? 1 : 0);
}
int host_proc_has_shim(uint32_t magic) {
    uint32_t i = magic & 0xFFFFFFu;
    pthread_mutex_lock(&procs_lock);
    int present = (magic & 0xFF000000u) == MAGIC_PROC && i < proc_count && procs[i].fn != NULL;
    pthread_mutex_unlock(&procs_lock);
    return present;
}
