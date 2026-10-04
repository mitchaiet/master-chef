/* Source-only DirectInput COM/input transitions with deterministic controller.
 * clang -O2 -pthread -I native/EngineHost -I native/EngineReuse \
 *   native/EngineHost/tests/test_dinput_lifecycle.c -lm -o /tmp/di-life */
#include <assert.h>
#include <stdarg.h>
#ifdef IO_LIFETIME_SOURCE
#include IO_LIFETIME_SOURCE
#else
#include "../dinput8.c"
#endif
#ifndef DIERR_OUTOFMEMORY
#define DIERR_OUTOFMEMORY 0x8007000Eu
#endif

uint8_t *engine_flat_base;
static uint32_t heap_next = 0x10000, frees;
static int fail_allocation;
static HostGCSnapshot controller;
static unsigned input_lost_logs;
void host_log(const char *fmt, ...) { if (strstr(fmt, "Acquire -> input lost")) input_lost_logs++; }
_Noreturn void host_exit(int code) { (void)code; assert(!"capacity must not terminate the host"); abort(); }
uint32_t guest_alloc(uint32_t n) {
    if (fail_allocation) { fail_allocation = 0; return 0; }
    uint32_t p = heap_next; heap_next += (n+15u)&~15u; assert(heap_next < 1024*1024); return p;
}
void guest_free(uint32_t p) { assert(p); frees++; }
uint32_t host_proc_address(const char *dll, const char *name) { (void)dll; (void)name; return 0; }
uint64_t host_monotonic_ns(void) { return 1000000000ull; }
uint32_t host_call_guest(EngineCPU *cpu, uint32_t fn, int n, const uint32_t *args, int pops) {
    (void)cpu; (void)fn; (void)n; (void)args; (void)pops; return 0;
}
void hostgc_init(void) {}
bool hostgc_poll(HostGCSnapshot *out) { *out = controller; return out->connected; }
bool hostgc_connected(void) { return controller.connected; }
static uint32_t call(HostShim fn, unsigned count, ...) {
    EngineCPU cpu = {0}; cpu.gpr[4] = 0x1000; S32(0x1000, 0x1234);
    va_list args; va_start(args, count);
    for (unsigned i = 0; i < count; i++) S32(0x1004+4*i, va_arg(args, uint32_t));
    va_end(args); fn(&cpu); assert(cpu.pc == 0x1234); return cpu.gpr[0];
}
static void lifecycle(void) {
    DIObj *root = obj_new(0, 0); assert(root); uint32_t root_guest = root->guest, stale = 0;
    S32(0x200, GUID_Joystick_D1);
    for (unsigned i = 0; i < 256; i++) {
        assert(call(shim_di_3, 4, root_guest, 0x200u, 0x220u, 0u) == DI_OK);
        uint32_t g = G32(0x220); assert(!obj_from_guest(stale));
        assert(call(shim_dev_1, 1, g) == 2);
        assert(call(shim_dev_2, 1, g) == 1 && obj_from_guest(g));
        assert(call(shim_dev_2, 1, g) == 0 && !obj_from_guest(g)); stale = g;
    }
    uint32_t before = frees;
    assert(call(shim_di_4, 5, root_guest, 4u, 1u, 0u, 0u) == DI_OK && frees == before+1);
    fail_allocation = 1;
    assert(call(shim_di_3, 4, root_guest, 0x200u, 0x220u, 0u) == DIERR_OUTOFMEMORY && G32(0x220) == 0);
    DIObj *all[62];
    for (unsigned i = 0; i < 62; i++) { all[i] = obj_new(DEV_MOUSE, 0); assert(all[i]); }
    assert(call(shim_di_3, 4, root_guest, 0x200u, 0x220u, 0u) == DIERR_OUTOFMEMORY && G32(0x220) == 0);
    assert(call(shim_DirectInput8Create, 5, 0u, 0x800u, 0u, 0x220u, 0u) == DIERR_OUTOFMEMORY && G32(0x220) == 0);
    for (unsigned i = 0; i < 62; i++) assert(call(shim_dev_2, 1, all[i]->guest) == 0);
    assert(call(shim_di_2, 1, root_guest) == 0 && !obj_from_guest(root_guest));
    puts("PASS 256 COM lifetimes, stale identity rejection, enumeration cleanup and recoverable full table");
}
static void input_transitions(void) {
    setenv("HALO_PAD2KEY", "0", 1); controller = (HostGCSnapshot){.connected = true};
    DIObj *joy = obj_new(DEV_JOYSTICK, 0); assert(joy); uint32_t g = joy->guest;
    joy->buffer_size = 2; joy->fmt_count = 1; joy->fmt[0] = (FmtObj){0, SRC_BUTTON, HOSTGC_BTN_A}; joy->data_size = 1;
    assert(call(shim_dev_7, 1, g) == DI_OK);
    for (unsigned i = 0; i < 5; i++) { controller.buttons[HOSTGC_BTN_A] = !(i&1); assert(call(shim_dev_25, 1, g) == DI_OK); }
    S32(0x240, 2);
    assert(call(shim_dev_10, 5, g, 16u, 0x300u, 0x240u, DIGDD_PEEK) == DI_BUFFEROVERFLOW);
    assert(G32(0x240) == 2 && G32(0x304) == 0 && G32(0x314) == 0x80);
    controller = (HostGCSnapshot){0};
    assert(call(shim_dev_9, 3, g, 1u, 0x300u) == DIERR_INPUTLOST && G8(0x300) == 0);
    assert(!joy->buf_count && !joy->buf_overflowed && !joy->acquired);
    controller = (HostGCSnapshot){.connected = true};
    assert(call(shim_dev_25, 1, g) == DIERR_NOTACQUIRED);
    assert(call(shim_dev_7, 1, g) == DI_OK);
    S32(0x240, 2); assert(call(shim_dev_10, 5, g, 16u, 0x300u, 0x240u, 0u) == DI_OK && G32(0x240) == 0);
    assert(call(shim_dev_2, 1, g) == 0);

    unsetenv("HALO_PAD2KEY");
    DIObj *key = obj_new(DEV_KEYBOARD, 0); assert(key); g = key->guest; key->buffer_size = 8;
    assert(call(shim_dev_7, 1, g) == DI_OK);
    controller.buttons[HOSTGC_BTN_A] = true; controller.rt = 1.f;
    S32(0x240, 8); assert(call(shim_dev_10, 5, g, 16u, 0x300u, 0x240u, 0u) == DI_OK);
    assert(G32(0x240) == 1 && G32(0x300) == 0x39 && G32(0x304) == 0x80);
    uint8_t mouse[8]; host_dinput_mouse_buttons_state(mouse); assert(mouse[0] == 0x80);
    S16(0x00718FA6, 1); /* gameplay -> pause menu releases jump and presses Enter */
    S32(0x240, 8); assert(call(shim_dev_10, 5, g, 16u, 0x300u, 0x240u, 0u) == DI_OK);
    assert(G32(0x240) == 2 && G32(0x300) == 0x1C && G32(0x304) == 0x80 && G32(0x310) == 0x39 && G32(0x314) == 0);
    host_dinput_mouse_buttons_state(mouse); assert(mouse[0] == 0);
    controller = (HostGCSnapshot){0};
    S32(0x240, 8); assert(call(shim_dev_10, 5, g, 16u, 0x300u, 0x240u, 0u) == DI_OK);
    assert(G32(0x240) == 1 && G32(0x300) == 0x1C && G32(0x304) == 0);
    S16(0x00718FA6, 0); host_dinput_mouse_buttons_state(mouse); assert(mouse[0] == 0);
    assert(call(shim_dev_8, 1, g) == DI_OK && !key->have_key_last && !key->buf_count);
    assert(call(shim_dev_2, 1, g) == 0);
    puts("PASS overflow/peek, disconnect release, reconnect/reacquire and gameplay/pause key/mouse transitions");
}
/* Halo retries Acquire every frame while the pad is away. Each retry is
 * refused and counted; only the first of an absence is logged. */
static void acquire_retries(void) {
    setenv("HALO_PAD2KEY", "0", 1); controller = (HostGCSnapshot){0};
    DIObj *joy = obj_new(DEV_JOYSTICK, 0); assert(joy); uint32_t g = joy->guest;
    uint64_t lost = host_dinput_acquire_lost(); input_lost_logs = 0;
    for (unsigned i = 0; i < 300; i++) assert(call(shim_dev_7, 1, g) == DIERR_INPUTLOST && !joy->acquired);
    assert(input_lost_logs == 1 && host_dinput_acquire_lost() == lost + 300);
    controller = (HostGCSnapshot){.connected = true};
    assert(call(shim_dev_7, 1, g) == DI_OK && joy->acquired);
    controller = (HostGCSnapshot){0};
    assert(call(shim_dev_25, 1, g) == DIERR_INPUTLOST && !joy->acquired);
    for (unsigned i = 0; i < 300; i++) assert(call(shim_dev_7, 1, g) == DIERR_INPUTLOST);
    assert(input_lost_logs == 2 && host_dinput_acquire_lost() == lost + 600);
    assert(call(shim_dev_2, 1, g) == 0);
    unsetenv("HALO_PAD2KEY");
    puts("PASS Acquire while the pad is away: every retry refused and counted, one log line per absence");
}
static void multiplayer_menu(void) {
    /* A network game keeps running while its root menu owns local input.
     * It never increments the campaign pause counter. */
    S8(0x00718FC9, 0); S16(0x00718FA6, 0); S16(0x00719720, 1);
    S32(0x00718F94, 0);
    controller = (HostGCSnapshot){.connected=true, .rt=1.f, .ly=-1.f};
    controller.buttons[HOSTGC_BTN_A] = true;
    controller.buttons[HOSTGC_BTN_RSHOULDER] = true;
    uint8_t keys[256], mouse[8];
    host_dinput_keyboard_state(keys); host_dinput_mouse_buttons_state(mouse);
    assert(!host_dinput_menu_active() && keys[0x39] && keys[0x21] && !keys[0x1C] && mouse[0]);

    S32(0x00718F94, 0x70000); /* original UI root, independent of pause depth */
    assert(host_dinput_menu_active());
    host_dinput_keyboard_state(keys); host_dinput_mouse_buttons_state(mouse);
    assert(keys[0x1C] && keys[0xD0] && !keys[0x39] && !keys[0x21] && !mouse[0]);
    controller.buttons[HOSTGC_BTN_B] = true;
    host_dinput_keyboard_state(keys); assert(keys[0x01] && !keys[0x1D]);
    host_mouse_buttons[0] = 0x80; /* preserve an actual pointer click */
    host_dinput_mouse_buttons_state(mouse); assert(mouse[0] == 0x80);
    host_mouse_buttons[0] = 0;
    setenv("HALO_PAD2KEY", "0", 1);
    HostGCSnapshot joystick;
    assert(joy_read_gameplay_snapshot(&joystick) && joystick.connected);
    assert(!joystick.buttons[HOSTGC_BTN_A] && !joystick.ly && !joystick.rt);
    unsetenv("HALO_PAD2KEY");

    S32(0x00718F94, 0); controller.buttons[HOSTGC_BTN_B] = false;
    host_dinput_keyboard_state(keys); host_dinput_mouse_buttons_state(mouse);
    assert(!host_dinput_menu_active() && keys[0x39] && !keys[0x1C] && !keys[0xD0] && mouse[0]);
    S8(0x00718FC9, 1); assert(host_dinput_menu_active()); S8(0x00718FC9, 0);
    S16(0x00718FA6, 2); assert(host_dinput_menu_active()); S16(0x00718FA6, 0);
    S16(0x00719720, 0); controller = (HostGCSnapshot){0};
    puts("PASS multiplayer root menu owns confirm/back/navigation/clicks; gameplay resumes on close");
}
int main(void) {
    /* The app's first diagnostics refresh runs before the engine starts. */
    assert(engine_flat_base == NULL);
    assert(host_dinput_menu_active() == 0);
    engine_flat_base = calloc(1, 8*1024*1024); assert(engine_flat_base);
    lifecycle(); input_transitions(); acquire_retries(); multiplayer_menu(); free(engine_flat_base);
    puts("PASS source-only DirectInput lifecycle; no controller/device acceptance implied");
}
