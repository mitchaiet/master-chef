/* DirectInput 8 (ANSI) for the native Halo host.
 *
 * System keyboard + mouse (host may fill their globals) plus a REAL joystick
 * device backed by Apple GameController (see gamecontroller.h/.m).  The joystick
 * path implements the pieces Halo actually drives: EnumDevices reports the pad
 * when one is attached, CreateDevice hands back a joystick device, SetDataFormat
 * is *interpreted* (we record each object Halo asks for by GUID/type+offset),
 * GetDeviceState fills exactly that layout from a freshly-polled controller
 * snapshot, GetDeviceData serves buffered relative events, and Acquire/Unacquire
 * /disconnect release the device.  Analog sticks -> axes, triggers -> Z/Rz axes
 * and buttons, dpad -> POV + buttons, face/shoulder/stick buttons -> buttons. */
#include "host.h"
#include "gamecontroller.h"
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>
static _Atomic uint64_t diagnostic_gamepad_reads, diagnostic_keyboard_events, diagnostic_acquire_lost;
uint64_t host_dinput_keyboard_events(void) { return atomic_load(&diagnostic_keyboard_events); }
uint64_t host_dinput_gamepad_reads(void) { return atomic_load(&diagnostic_gamepad_reads); }
uint64_t host_dinput_acquire_lost(void) { return atomic_load(&diagnostic_acquire_lost); }

#define DI_OK 0
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_INPUTLOST    0x8007001Eu
#define DIERR_NOTACQUIRED  0x8007000Cu
#define DIERR_OTHERAPPHASPRIO 0x80070005u
#define DIERR_INVALIDPARAM     0x80070057u
#define DIERR_OUTOFMEMORY      0x8007000Eu
#define DIERR_OBJECTNOTFOUND  0x80070002u
#define DI_BUFFEROVERFLOW     1u
#define DIENUM_STOP           0u
#define DIGDD_PEEK            1u
#define DIEDFL_FORCEFEEDBACK  0x00000100u

/* DirectInput object GUIDs (Data1 word identifies the axis/button class). */
#define GUID_XAxis_D1  0xA36D02E0u
#define GUID_YAxis_D1  0xA36D02E1u
#define GUID_ZAxis_D1  0xA36D02E2u
#define GUID_RzAxis_D1 0xA36D02E3u
#define GUID_Slider_D1 0xA36D02E4u
#define GUID_Button_D1 0xA36D02F0u
#define GUID_POV_D1    0xA36D02F2u
#define GUID_RxAxis_D1 0xA36D02F4u
#define GUID_RyAxis_D1 0xA36D02F5u

/* DIDFT_* type flags (low bits of DIOBJECTDATAFORMAT.dwType). */
#define DIDFT_AXIS   0x00000003u
#define DIDFT_BUTTON 0x0000000Cu
#define DIDFT_POV    0x00000010u
#define DIDFT_GETINSTANCE(t) (((t) >> 8) & 0xFFFFu)
#define DIDFT_ANYINSTANCE 0xFFFFu

/* Semantic axis/POV/button identity for one parsed data-format object. */
enum { SRC_NONE = 0, SRC_X, SRC_Y, SRC_Z, SRC_RX, SRC_RY, SRC_RZ, SRC_SLIDER, SRC_POV, SRC_BUTTON };

typedef struct { const char *name; uint8_t argc; } Method;
static const Method m_di[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"CreateDevice",4},{"EnumDevices",5},{"GetDeviceStatus",2},{"RunControlPanel",3},{"Initialize",3},{"FindDevice",4},{"EnumDevicesBySemantics",6},{"ConfigureDevices",5} };
static const Method m_dev[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCapabilities",2},{"EnumObjects",4},{"GetProperty",3},{"SetProperty",3},{"Acquire",1},{"Unacquire",1},{"GetDeviceState",3},{"GetDeviceData",5},{"SetDataFormat",2},{"SetEventNotification",2},{"SetCooperativeLevel",3},{"GetObjectInfo",4},{"GetDeviceInfo",2},{"RunControlPanel",3},{"Initialize",4},{"CreateEffect",5},{"EnumEffects",4},{"GetEffectInfo",3},{"GetForceFeedbackState",2},{"SendForceFeedbackCommand",2},{"EnumCreatedEffectObjects",4},{"Escape",2},{"Poll",1},{"SendDeviceData",5},{"EnumEffectsInFile",5},{"WriteEffectToFile",5},{"BuildActionMap",4},{"SetActionMap",4},{"GetImageInfo",2} };
#define N(a) (int)(sizeof(a)/sizeof((a)[0]))
enum { DEV_KEYBOARD = 1, DEV_MOUSE, DEV_JOYSTICK };

/* One entry of Halo's interpreted data format: where (offset) and what (src/inst). */
#define MAX_FMT_OBJS 64
typedef struct { uint32_t ofs; uint8_t src; uint8_t inst; } FmtObj;

/* A buffered relative event (DIDEVICEOBJECTDATA: dwOfs,dwData,dwTimeStamp,dwSequence,uAppData). */
typedef struct { uint32_t ofs, data, time, seq; } BufEvent;
#define MAX_BUF_EVENTS 128

typedef struct {
    int kind; uint32_t guest; int refs; int acquired;
    uint32_t buffer_size;                 /* DIPROP_BUFFERSIZE, in events */
    /* joystick data-format interpretation */
    FmtObj fmt[MAX_FMT_OBJS]; int fmt_count; uint32_t data_size;
    int32_t axis_min, axis_max;           /* DIPROP_RANGE (default 0..65535) */
    uint32_t deadzone;                    /* DIPROP_DEADZONE, 0..10000 */
    /* joystick buffered-event ring + last snapshot for edge detection */
    BufEvent buf[MAX_BUF_EVENTS]; int buf_head, buf_tail, buf_count, buf_overflowed; uint32_t event_seq;
    HostGCSnapshot last; int have_last;
    uint8_t key_last[256]; int have_key_last;
    int logged_poll_failure, logged_acquire_failure;
} DIObj;
static DIObj objs[64]; static int obj_count = 1;
static uint32_t vt_di, vt_dev;
uint8_t host_keyboard_state[256];            /* DIK_* scan codes -> 0x80 when down; the host may fill these */
int32_t host_mouse_dx, host_mouse_dy, host_mouse_dz; uint8_t host_mouse_buttons[8];

/* A stable GUID_Joystick we hand out in EnumDevices and match in CreateDevice. */
#define GUID_Joystick_D1 0x6F1D2B70u

/* ---- controller snapshot -> DirectInput value conversions ------------------
 * DirectInput axes are unsigned, centred; sticks map [-1,+1] -> [min,max] with
 * the DInput sign convention (screen Y is down, so we invert the GC "y up").
 * Triggers map [0,1] -> [min,max]. */
static int32_t round_axis(double v) {
    if (v <= (double)INT32_MIN) return INT32_MIN;
    if (v >= (double)INT32_MAX) return INT32_MAX;
    return (int32_t)(v < 0.0 ? v - 0.5 : v + 0.5);
}
static float stick_deadzone(float v, uint32_t deadzone) {
    if (v < -1.f) v = -1.f; else if (v > 1.f) v = 1.f;
    float dz = deadzone > 10000u ? 1.f : (float)deadzone / 10000.f;
    float mag = v < 0.f ? -v : v;
    if (mag <= dz || dz >= 1.f) return 0.f;
    mag = (mag - dz) / (1.f - dz);
    return v < 0.f ? -mag : mag;
}
static float trigger_deadzone(float v, uint32_t deadzone) {
    if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
    float dz = deadzone > 10000u ? 1.f : (float)deadzone / 10000.f;
    if (v <= dz || dz >= 1.f) return 0.f;
    return (v - dz) / (1.f - dz);
}
static int32_t axis_from_stick(float v, int32_t mn, int32_t mx, uint32_t deadzone) {
    v = stick_deadzone(v, deadzone);
    double t = ((double)v + 1.0) * 0.5;   /* 0..1 */
    return round_axis((double)mn + t * ((double)mx - (double)mn));
}
static int32_t axis_from_trigger(float v, int32_t mn, int32_t mx, uint32_t deadzone) {
    v = trigger_deadzone(v, deadzone);
    return round_axis((double)mn + (double)v * ((double)mx - (double)mn));
}
/* dpad -> DirectInput POV: hundredths of a degree, clockwise from north; -1 (0xFFFFFFFF) = centred. */
static uint32_t pov_from_dpad(const HostGCSnapshot *s) {
    int up = s->dpad_up, dn = s->dpad_down, lf = s->dpad_left, rt = s->dpad_right;
    if (up && rt) return 4500;  if (rt && dn) return 13500;
    if (dn && lf) return 22500; if (lf && up) return 31500;
    if (up) return 0; if (rt) return 9000; if (dn) return 18000; if (lf) return 27000;
    return 0xFFFFFFFFu;
}
static int32_t axis_value_for_src(const HostGCSnapshot *s, int src, int32_t mn, int32_t mx, uint32_t deadzone) {
    int32_t mid = axis_from_stick(0.f, mn, mx, 0);
    switch (src) {
    case SRC_X:      return axis_from_stick(s->lx, mn, mx, deadzone);
    case SRC_Y:      return axis_from_stick(-s->ly, mn, mx, deadzone);   /* GC y-up -> DInput y-down */
    case SRC_RX:     return axis_from_stick(s->rx, mn, mx, deadzone);
    case SRC_RY:     return axis_from_stick(-s->ry, mn, mx, deadzone);
    /* Expose both analog triggers independently in the conventional spare axes. */
    case SRC_Z:      return axis_from_trigger(s->rt, mn, mx, deadzone);
    case SRC_RZ:     return axis_from_trigger(s->lt, mn, mx, deadzone);
    case SRC_SLIDER: return mid;
    default:         return mid;
    }
}

static uint32_t make_vtable(const char *iface, const Method *m, int n) {
    uint32_t vt = guest_alloc(4u * (uint32_t)n); char name[96];
    for (int i = 0; i < n; i++) { snprintf(name, sizeof name, "%s::%s", iface, m[i].name); S32(vt + 4u * (uint32_t)i, host_proc_address("dinput8.dll", name)); }
    return vt;
}
static DIObj *obj_new(int kind, uint32_t vt) {
    int index = 1;
    while (index < obj_count && objs[index].refs) index++;
    if (index >= N(objs)) { host_log("dinput object table full"); return NULL; }
    uint32_t guest = guest_alloc(16);
    if (!guest) return NULL;
    DIObj *o = &objs[index]; memset(o, 0, sizeof *o); o->kind = kind; o->refs = 1; o->axis_min = 0; o->axis_max = 65535; o->guest = guest; S32(guest, vt); S32(guest + 4, (uint32_t)index);
    if (index == obj_count) obj_count++;
    return o;
}
static DIObj *obj_from_guest(uint32_t g) { if (!g) return NULL; uint32_t i = G32(g + 4); return (i && i < (uint32_t)obj_count && objs[i].refs && objs[i].guest == g) ? &objs[i] : NULL; }
static uint32_t obj_release(DIObj *o) {
    if (o->refs > 1) return (uint32_t)--o->refs;
    guest_free(o->guest);
    memset(o, 0, sizeof *o);
    return 0;
}

static void method_di(EngineCPU *cpu, DIObj *o, int i) {
    switch (i) {
    case 0: { uint32_t out = ARG(2); if (!out) RET_STDCALL(DIERR_INVALIDPARAM, 3); S32(out, o->guest); o->refs++; RET_STDCALL(DI_OK, 3); }
    case 1: RET_STDCALL(++o->refs, 1); case 2: RET_STDCALL(obj_release(o), 1);
    case 3: { uint32_t guid = ARG(1), out = ARG(2);
        if (!guid || !out) RET_STDCALL(DIERR_INVALIDPARAM, 4);
        S32(out, 0); uint32_t d1 = G32(guid);
        int kind = d1 == 0x6F1D2B61u ? DEV_KEYBOARD : d1 == 0x6F1D2B60u ? DEV_MOUSE : d1 == GUID_Joystick_D1 ? DEV_JOYSTICK : 0;
        if (!kind) { host_log("dinput8 CreateDevice(guid %08X) -> not registered", d1); S32(out, 0); RET_STDCALL(DIERR_DEVICENOTREG, 4); }
        if (kind == DEV_JOYSTICK) hostgc_init();
        DIObj *d = obj_new(kind, vt_dev); if (!d) RET_STDCALL(DIERR_OUTOFMEMORY, 4); S32(out, d->guest);
        host_log("dinput8 CreateDevice -> %s", kind == DEV_KEYBOARD ? "keyboard" : kind == DEV_MOUSE ? "mouse" : "joystick"); RET_STDCALL(DI_OK, 4); }
    case 4: { uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3), flags = ARG(4);
        /* DI8DEVCLASS_GAMECTRL == 4, DI8DEVTYPE_GAMEPAD low byte == 0x15.  This
         * is one stable virtual bridge device, so enumerate it even before an
         * Apple controller appears. Halo enumerates only during startup; the
         * device's status/acquire/poll calls remain truthful and can recover
         * after a later GameController connection. It has no force feedback. */
        int want_pad = (type == 0 || type == 4 || (type & 0xFF) == 0x15 || (type & 0xFF) == 0x14 || (type & 0xFF) == 0x04);
        hostgc_init();
        if (want_pad && !(flags & DIEDFL_FORCEFEEDBACK)) {
            uint32_t di = guest_alloc(580); memset(GPTR(di), 0, 580);
            S32(di, 580);                              /* dwSize */
            S32(di + 4, GUID_Joystick_D1);             /* guidInstance.Data1 */
            S32(di + 20, GUID_Joystick_D1);            /* guidProduct.Data1 */
            S32(di + 36, 0x00010115u);                 /* dwDevType: GAMEPAD */
            strcpy((char *)GPTR(di + 40), "Halo Vision Gamepad");
            strcpy((char *)GPTR(di + 300), "Halo Vision Gamepad");
            uint32_t args[2] = { di, ref };
            host_log("dinput8 EnumDevices(type %08X flags %08X): reporting virtual gamepad bridge (%s)",
                     type, flags, hostgc_connected() ? "controller connected" : "awaiting controller");
            uint32_t keep_going = host_call_guest(cpu, cb, 2, args, 1);
            guest_free(di);
            (void)keep_going;
        } else {
            host_log("dinput8 EnumDevices(type %08X flags %08X): virtual gamepad excluded", type, flags);
        }
        RET_STDCALL(DI_OK, 5); }
    case 5: { uint32_t guid = ARG(1); uint32_t d1 = guid ? G32(guid) : 0;
        if (d1 == GUID_Joystick_D1) RET_STDCALL(hostgc_connected() ? DI_OK : DIERR_INPUTLOST, 2);
        RET_STDCALL(DI_OK, 2); }
    case 6: RET_STDCALL(DI_OK, 3); case 7: RET_STDCALL(DI_OK, 3);
    case 8: RET_STDCALL(DIERR_DEVICENOTREG, 4); case 9: RET_STDCALL(DI_OK, 6); case 10: RET_STDCALL(DI_OK, 5);
    default: RET_STDCALL(0x80004001u, m_di[i].argc);
    }
}
/* Interpret Halo's DIDATAFORMAT (SetDataFormat) into our FmtObj table so
 * GetDeviceState fills exactly the offsets/objects Halo declared. */
static int joy_parse_format(DIObj *o, uint32_t df) {
    o->fmt_count = 0;
    o->data_size = 0;
    if (!df || G32(df) < 24 || G32(df + 4) < 16) return 0;
    o->data_size = G32(df + 12);            /* dwDataSize */
    uint32_t num = G32(df + 16);            /* dwNumObjs  */
    uint32_t rgodf = G32(df + 20);          /* DIOBJECTDATAFORMAT* */
    uint32_t objsz = G32(df + 4);           /* dwObjSize (usually 16) */
    if (!o->data_size || o->data_size > 4096 || (num && !rgodf)) { o->data_size = 0; return 0; }
    uint32_t axis_ord = 0, pov_ord = 0, button_ord = 0;
    static const uint8_t axis_order[] = { SRC_X, SRC_Y, SRC_Z, SRC_RX, SRC_RY, SRC_RZ, SRC_SLIDER, SRC_SLIDER };
    for (uint32_t k = 0; k < num && o->fmt_count < MAX_FMT_OBJS; k++) {
        uint32_t e = rgodf + k * objsz;
        uint32_t pguid = G32(e);            /* GUID* or NULL (match by type) */
        uint32_t ofs   = G32(e + 4);
        uint32_t type  = G32(e + 8);
        uint32_t raw_inst = DIDFT_GETINSTANCE(type);
        uint8_t src = SRC_NONE, inst = 0;
        int object_class = (type & DIDFT_POV) ? SRC_POV :
                           (type & DIDFT_BUTTON) ? SRC_BUTTON :
                           (type & DIDFT_AXIS) ? SRC_X : SRC_NONE;
        if (pguid) {
            uint32_t d1 = G32(pguid);
            src = d1 == GUID_XAxis_D1 ? SRC_X : d1 == GUID_YAxis_D1 ? SRC_Y :
                  d1 == GUID_ZAxis_D1 ? SRC_Z : d1 == GUID_RxAxis_D1 ? SRC_RX :
                  d1 == GUID_RyAxis_D1 ? SRC_RY : d1 == GUID_RzAxis_D1 ? SRC_RZ :
                  d1 == GUID_Slider_D1 ? SRC_SLIDER : d1 == GUID_POV_D1 ? SRC_POV :
                  d1 == GUID_Button_D1 ? SRC_BUTTON : SRC_NONE;
        }
        if (!pguid || src == SRC_NONE) src = object_class;

        /* Halo's runtime-built c_dfDIJoystick declares all 80 entries with
         * DIDFT_ANYINSTANCE.  DirectInput binds those in occurrence order; 255
         * is not a real button or axis index. */
        if (src == SRC_BUTTON || object_class == SRC_BUTTON) {
            uint32_t n = raw_inst == DIDFT_ANYINSTANCE ? button_ord : raw_inst;
            button_ord++;
            if (n >= HOSTGC_BUTTON_COUNT) continue;
            src = SRC_BUTTON; inst = (uint8_t)n;
        } else if (src == SRC_POV || object_class == SRC_POV) {
            uint32_t n = raw_inst == DIDFT_ANYINSTANCE ? pov_ord : raw_inst;
            pov_ord++;
            if (n != 0) continue;
            src = SRC_POV; inst = 0;
        } else if (src >= SRC_X && src <= SRC_SLIDER) {
            uint32_t n = raw_inst == DIDFT_ANYINSTANCE ? axis_ord : raw_inst;
            axis_ord++;
            if (!pguid) {
                if (n >= sizeof axis_order) continue;
                src = axis_order[n];
            }
            inst = (uint8_t)n;
        }
        if (src == SRC_NONE) continue;
        o->fmt[o->fmt_count].ofs = ofs; o->fmt[o->fmt_count].src = src; o->fmt[o->fmt_count].inst = inst; o->fmt_count++;
    }
    host_log("dinput8 joystick SetDataFormat: %d objects, dataSize %u", o->fmt_count, o->data_size);
    return 1;
}

/* Fill a GetDeviceState buffer of dwSize n from a controller snapshot per the
 * parsed format.  Axes default to their range midpoint; POV to centred. */
static void joy_fill_state(DIObj *o, const HostGCSnapshot *s, uint32_t buf, uint32_t n) {
    memset(GPTR(buf), 0, n);
    /* Default all mapped axes to midpoint / POV to centred even without objects. */
    for (int k = 0; k < o->fmt_count; k++) {
        FmtObj *f = &o->fmt[k];
        if (f->src == SRC_BUTTON) {
            if (f->ofs >= n) continue;
            int down = f->inst < HOSTGC_BUTTON_COUNT ? s->buttons[f->inst] : 0;
            S8(buf + f->ofs, down ? 0x80 : 0x00);
        } else if (f->src == SRC_POV) {
            if (f->ofs > n || n - f->ofs < 4u) continue;
            S32(buf + f->ofs, pov_from_dpad(s));
        } else {
            if (f->ofs > n || n - f->ofs < 4u) continue;
            S32(buf + f->ofs, (uint32_t)axis_value_for_src(s, f->src, o->axis_min, o->axis_max, o->deadzone));
        }
    }
}

/* Diff two snapshots into buffered relative events (for GetDeviceData). */
static void joy_clear_buffer(DIObj *o) {
    o->buf_head = o->buf_tail = o->buf_count = o->buf_overflowed = 0;
}
static void joy_queue_event(DIObj *o, uint32_t ofs, uint32_t data) {
    uint32_t cap = o->buffer_size < MAX_BUF_EVENTS ? o->buffer_size : MAX_BUF_EVENTS;
    if (!cap) return;
    if ((uint32_t)o->buf_count >= cap) {
        o->buf_head = (o->buf_head + 1) % MAX_BUF_EVENTS;
        o->buf_count--;
        o->buf_overflowed = 1;
    }
    BufEvent *e = &o->buf[o->buf_tail];
    e->ofs = ofs; e->data = data;
    e->time = (uint32_t)(host_monotonic_ns() / 1000000ull);
    e->seq = ++o->event_seq;
    o->buf_tail = (o->buf_tail + 1) % MAX_BUF_EVENTS;
    o->buf_count++;
}
/* 00718F94 is the original active UI widget root (published by 0049A6C1,
 * removed by 00497E6D). It owns navigation even when the game keeps running.
 * The 00718FA6 pause counter alone cannot detect non-pausing multiplayer
 * menus: creation/teardown gate it on the widget's pause flag (+13 at
 * 00499907/00497CD8), as well as the game mode and shell state.
 * Read the root without dereferencing it; HUD rendering uses a separate path. */
int host_dinput_menu_active(void) {
    /* SwiftUI can query this from the diagnostics panel before enginevision_start
     * has reserved the guest address space.  Never dereference guest VAs in that
     * pre-start state. */
    if (!engine_flat_base) return 0;
    return G8(0x00718FC9) != 0 || G32(0x00718F94) != 0 || G16(0x00718FA6) != 0;
}
static bool gamepad_keyboard_mouse_enabled(void) {
    const char *option = getenv("HALO_PAD2KEY");
    return !option || strcmp(option, "0") != 0;
}
/* Original 1.10 executable UI-shell lifecycle: 004C8930 sets 00718FC9
 * after selecting levels\\ui\\ui; 004C8B40 clears it on shell teardown.
 * The shell and widget-root state restrict navigation to menus. Never write
 * engine state, and never emit Cross=Enter during ordinary gameplay. */
/* DirectInput owns arrow/menu navigation; USER32 also samples Return/Escape
 * for original text dialogs that do not consume the DirectInput key state. */
void host_dinput_keyboard_state(uint8_t out[256]) {
    memcpy(out, host_keyboard_state, 256);
    HostGCSnapshot pad = {0};
    if (!hostgc_poll(&pad)) return;
    /* Options/Start must also reach cinematic skip and the in-game pause menu.
     * Cross/Enter and directional keys remain restricted to active menus. */
    if (pad.buttons[HOSTGC_BTN_MENU]) out[0x01] |= 0x80;
    if (!host_dinput_menu_active()) {
        if (!gamepad_keyboard_mouse_enabled()) return;
        /* PC's E/Use also acknowledges the Normal-difficulty cryotube prompt.
         * Feed the original keyboard action so the tutorial sees the same
         * press/release edge as a physical E key. */
        if (pad.buttons[HOSTGC_BTN_X]) { out[0x12] |= 0x80; out[0x13] |= 0x80; } /* use/reload */
        if (pad.buttons[HOSTGC_BTN_A]) out[0x39] |= 0x80; /* Space: jump */
        if (pad.buttons[HOSTGC_BTN_B] || pad.buttons[HOSTGC_BTN_LTHUMB]) out[0x1D] |= 0x80; /* Ctrl: crouch */
        if (pad.buttons[HOSTGC_BTN_Y]) out[0x0F] |= 0x80; /* Tab: switch weapon */
        if (pad.buttons[HOSTGC_BTN_RSHOULDER]) out[0x21] |= 0x80; /* F: melee */
        if (pad.buttons[HOSTGC_BTN_LTRIGGER] || pad.lt > 0.25f || pad.buttons[HOSTGC_BTN_RTHUMB]) out[0x2C] |= 0x80; /* Z: cycle zoom */
        if (pad.dpad_up || pad.buttons[HOSTGC_BTN_DPAD_UP]) out[0x10] |= 0x80; /* Q: flashlight */
        if (pad.dpad_down || pad.buttons[HOSTGC_BTN_DPAD_DOWN]) out[0x22] |= 0x80; /* G: switch grenade */
        if (pad.dpad_left || pad.buttons[HOSTGC_BTN_DPAD_LEFT]) out[0x13] |= 0x80; /* R: reload only */
        if (pad.dpad_right || pad.buttons[HOSTGC_BTN_DPAD_RIGHT]) out[0x2D] |= 0x80; /* X: exchange weapon */
        if (pad.buttons[HOSTGC_BTN_OPTIONS]) out[0x3B] |= 0x80; /* F1: score */
        return;
    }
    bool up = pad.dpad_up || pad.ly > 0.55f;
    bool down = pad.dpad_down || pad.ly < -0.55f;
    bool left = pad.dpad_left || pad.lx < -0.55f;
    bool right = pad.dpad_right || pad.lx > 0.55f;
    if (up && !down) out[0xC8] |= 0x80;
    if (down && !up) out[0xD0] |= 0x80;
    if (left && !right) out[0xCB] |= 0x80;
    if (right && !left) out[0xCD] |= 0x80;
    if (pad.buttons[HOSTGC_BTN_A]) out[0x1C] |= 0x80;
    if (pad.buttons[HOSTGC_BTN_B]) out[0x01] |= 0x80;
}
void host_dinput_mouse_buttons_state(uint8_t out[8]) {
    memcpy(out, host_mouse_buttons, 8);
    if (host_dinput_menu_active() || !gamepad_keyboard_mouse_enabled()) return;
    HostGCSnapshot pad = {0};
    if (!hostgc_poll(&pad)) return;
    if (pad.buttons[HOSTGC_BTN_RTRIGGER] || pad.rt > 0.25f) out[0] |= 0x80;
    if (pad.buttons[HOSTGC_BTN_LSHOULDER]) out[1] |= 0x80;
}
static void keyboard_buffer_events(DIObj *o) {
    uint8_t keys[256]; host_dinput_keyboard_state(keys);
    if (!o->buffer_size) return;
    if (!o->have_key_last) {
        memcpy(o->key_last, keys, sizeof o->key_last);
        o->have_key_last = 1;
        return;
    }
    for (uint32_t dik = 0; dik < 256; dik++) {
        uint8_t now = keys[dik] & 0x80u;
        uint8_t before = o->key_last[dik] & 0x80u;
        if (now != before) joy_queue_event(o, dik, now);
    }
    memcpy(o->key_last, keys, sizeof o->key_last);
}
static void joy_buffer_events(DIObj *o, const HostGCSnapshot *cur) {
    if (!o->buffer_size) return;
    if (!o->have_last) { o->last = *cur; o->have_last = 1; return; }
    for (int k = 0; k < o->fmt_count; k++) {
        FmtObj *f = &o->fmt[k];
        uint32_t nv, ov;
        if (f->src == SRC_BUTTON) {
            int nb = f->inst < HOSTGC_BUTTON_COUNT ? cur->buttons[f->inst] : 0;
            int ob = f->inst < HOSTGC_BUTTON_COUNT ? o->last.buttons[f->inst] : 0;
            if (nb == ob) continue; nv = nb ? 0x80 : 0; ov = ob ? 0x80 : 0;
        } else if (f->src == SRC_POV) {
            nv = pov_from_dpad(cur); ov = pov_from_dpad(&o->last); if (nv == ov) continue;
        } else {
            nv = (uint32_t)axis_value_for_src(cur, f->src, o->axis_min, o->axis_max, o->deadzone);
            ov = (uint32_t)axis_value_for_src(&o->last, f->src, o->axis_min, o->axis_max, o->deadzone);
            if (nv == ov) continue;
        }
        joy_queue_event(o, f->ofs, nv);
    }
    o->last = *cur;
}

/* Menus are driven through one keyboard navigation route even after the
 * original profile assigns a joystick. Reporting the same button through DI
 * joystick as well causes skipped choices. When PAD2KEY owns gameplay, also
 * neutralize the joystick route so assigned profile bindings cannot double-fire
 * actions or change the meaning of a button. PAD2KEY=0 retains native joystick. */
static bool joy_read_gameplay_snapshot(HostGCSnapshot *out) {
    if (!hostgc_poll(out)) return false;
    if (host_dinput_menu_active() || gamepad_keyboard_mouse_enabled()) {
        uint64_t sequence = out->sequence;
        memset(out, 0, sizeof *out); out->connected = true; out->sequence = sequence;
    }
    return true;
}
static uint32_t joy_poll_acquired(DIObj *o, HostGCSnapshot *out) {
    HostGCSnapshot cur;
    if (!joy_read_gameplay_snapshot(&cur)) {
        o->acquired = 0;
        o->have_last = 0;
        memset(&o->last, 0, sizeof o->last);
        joy_clear_buffer(o);
        if (out) memset(out, 0, sizeof *out);
        return DIERR_INPUTLOST;
    }
    joy_buffer_events(o, &cur);
    if (out) *out = cur;
    return DI_OK;
}

#define JOY_AXIS_COUNT 6u
static const uint32_t joy_axis_guids[] = {
    GUID_XAxis_D1, GUID_YAxis_D1, GUID_ZAxis_D1,
    GUID_RxAxis_D1, GUID_RyAxis_D1, GUID_RzAxis_D1
};
static const char *joy_axis_names[] = { "X Axis", "Y Axis", "Right Trigger", "Rx Axis", "Ry Axis", "Left Trigger" };
static const char *joy_button_names[HOSTGC_BUTTON_COUNT] = {
    "A / Cross", "B / Circle", "X / Square", "Y / Triangle",
    "Left Shoulder", "Right Shoulder", "Left Trigger", "Right Trigger",
    "Left Stick", "Right Stick", "Menu / Options", "Back / Create", "Home / Guide",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"
};

static int object_requested(uint32_t flags, uint32_t type) {
    if (!flags) return 1;
    if ((type & DIDFT_AXIS) && (flags & DIDFT_AXIS)) return 1;
    if ((type & DIDFT_BUTTON) && (flags & DIDFT_BUTTON)) return 1;
    if ((type & DIDFT_POV) && (flags & DIDFT_POV)) return 1;
    return 0;
}

static uint32_t joy_emit_object(EngineCPU *cpu, uint32_t callback, uint32_t ref,
                                uint32_t flags, uint32_t guid_d1, uint32_t ofs,
                                uint32_t type, const char *name) {
    if (!object_requested(flags, type)) return 1;
    uint32_t info = guest_alloc(316);
    memset(GPTR(info), 0, 316);
    S32(info, 316);                /* DIDEVICEOBJECTINSTANCEA.dwSize */
    S32(info + 4, guid_d1);        /* guidType.Data1 */
    S32(info + 20, ofs);           /* dwOfs */
    S32(info + 24, type);          /* dwType */
    S32(info + 28, 0x100u);        /* DIDOI_ASPECTPOSITION */
    snprintf((char *)GPTR(info + 32), 260, "%s", name);
    uint32_t args[2] = { info, ref };
    uint32_t result = host_call_guest(cpu, callback, 2, args, 1);
    guest_free(info);
    return result;
}

static uint32_t joy_enum_objects(EngineCPU *cpu, uint32_t callback, uint32_t ref, uint32_t flags) {
    if (!callback) return DIERR_INVALIDPARAM;
    for (uint32_t n = 0; n < JOY_AXIS_COUNT; n++) {
        uint32_t type = 0x00000002u | (n << 8); /* DIDFT_ABSAXIS */
        if (joy_emit_object(cpu, callback, ref, flags, joy_axis_guids[n], n * 4u, type, joy_axis_names[n]) == DIENUM_STOP)
            return DI_OK;
    }
    if (joy_emit_object(cpu, callback, ref, flags, GUID_POV_D1, 0x80, DIDFT_POV, "D-pad") == DIENUM_STOP)
        return DI_OK;
    for (uint32_t n = 0; n < HOSTGC_BUTTON_COUNT; n++) {
        uint32_t type = DIDFT_BUTTON | (n << 8);
        if (joy_emit_object(cpu, callback, ref, flags, GUID_Button_D1, 0xC0u + n, type, joy_button_names[n]) == DIENUM_STOP)
            return DI_OK;
    }
    return DI_OK;
}

static uint32_t joy_set_property(DIObj *o, uint32_t prop, uint32_t hdr) {
    if (!hdr || G32(hdr) < 20 || G32(hdr + 4) < 16) return DIERR_INVALIDPARAM;
    if (prop == 1) { /* DIPROP_BUFFERSIZE */
        uint32_t requested = G32(hdr + 16);
        o->buffer_size = requested < MAX_BUF_EVENTS ? requested : MAX_BUF_EVENTS;
        joy_clear_buffer(o);
        o->have_last = 0;
        o->have_key_last = 0;
        return DI_OK;
    }
    if (prop == 4) { /* DIPROP_RANGE */
        if (G32(hdr) < 24) return DIERR_INVALIDPARAM;
        int32_t mn = (int32_t)G32(hdr + 16), mx = (int32_t)G32(hdr + 20);
        if (mn >= mx) return DIERR_INVALIDPARAM;
        o->axis_min = mn; o->axis_max = mx;
        o->have_last = 0;
        return DI_OK;
    }
    if (prop == 5) { /* DIPROP_DEADZONE */
        uint32_t deadzone = G32(hdr + 16);
        if (deadzone > 10000u) return DIERR_INVALIDPARAM;
        o->deadzone = deadzone;
        o->have_last = 0;
        return DI_OK;
    }
    return DIERR_OBJECTNOTFOUND;
}

static uint32_t joy_get_property(DIObj *o, uint32_t prop, uint32_t hdr) {
    if (!hdr || G32(hdr) < 20 || G32(hdr + 4) < 16) return DIERR_INVALIDPARAM;
    if (prop == 1) S32(hdr + 16, o->buffer_size);
    else if (prop == 4) {
        if (G32(hdr) < 24) return DIERR_INVALIDPARAM;
        S32(hdr + 16, (uint32_t)o->axis_min); S32(hdr + 20, (uint32_t)o->axis_max);
    } else if (prop == 5) S32(hdr + 16, o->deadzone);
    else return DIERR_OBJECTNOTFOUND;
    return DI_OK;
}

static uint32_t joy_get_data(DIObj *o, uint32_t object_size, uint32_t data,
                             uint32_t inout, uint32_t flags) {
    if (!inout || (data && object_size < 16)) return DIERR_INVALIDPARAM;
    if (!o->acquired) { S32(inout, 0); return DIERR_NOTACQUIRED; }
    uint32_t polled = joy_poll_acquired(o, NULL);
    if (polled != DI_OK) { S32(inout, 0); return polled; }

    uint32_t wanted = G32(inout);
    uint32_t take = wanted < (uint32_t)o->buf_count ? wanted : (uint32_t)o->buf_count;
    int peek = (flags & DIGDD_PEEK) != 0;
    int index = o->buf_head;
    for (uint32_t n = 0; n < take; n++) {
        BufEvent *e = &o->buf[index];
        if (data) {
            uint32_t dst = data + n * object_size;
            S32(dst, e->ofs); S32(dst + 4, e->data);
            S32(dst + 8, e->time); S32(dst + 12, e->seq);
            if (object_size >= 20) S32(dst + 16, 0);
        }
        index = (index + 1) % MAX_BUF_EVENTS;
    }
    if (!peek) {
        o->buf_head = index;
        o->buf_count -= (int)take;
    }
    S32(inout, take);
    uint32_t result = o->buf_overflowed ? DI_BUFFEROVERFLOW : DI_OK;
    if (!peek) o->buf_overflowed = 0;
    return result;
}

static uint32_t keyboard_get_data(DIObj *o, uint32_t object_size, uint32_t data,
                                  uint32_t inout, uint32_t flags) {
    if (!inout || (data && object_size < 16)) return DIERR_INVALIDPARAM;
    if (!o->acquired) { S32(inout, 0); return DIERR_NOTACQUIRED; }
    keyboard_buffer_events(o);

    uint32_t wanted = G32(inout);
    uint32_t take = wanted < (uint32_t)o->buf_count ? wanted : (uint32_t)o->buf_count;
    int peek = (flags & DIGDD_PEEK) != 0;
    int index = o->buf_head;
    for (uint32_t n = 0; n < take; n++) {
        BufEvent *e = &o->buf[index];
        if (data) {
            uint32_t dst = data + n * object_size;
            S32(dst, e->ofs); S32(dst + 4, e->data);
            S32(dst + 8, e->time); S32(dst + 12, e->seq);
            if (object_size >= 20) S32(dst + 16, 0);
        }
        index = (index + 1) % MAX_BUF_EVENTS;
    }
    if (!peek) { o->buf_head = index; o->buf_count -= (int)take;
        if (data) atomic_fetch_add(&diagnostic_keyboard_events, take);
    }
    S32(inout, take);
    uint32_t result = o->buf_overflowed ? DI_BUFFEROVERFLOW : DI_OK;
    if (!peek) o->buf_overflowed = 0;
    return result;
}

static void method_dev(EngineCPU *cpu, DIObj *o, int i) {
    switch (i) {
    case 0: { uint32_t out = ARG(2); if (!out) RET_STDCALL(DIERR_INVALIDPARAM, 3); S32(out, o->guest); o->refs++; RET_STDCALL(DI_OK, 3); }
    case 1: RET_STDCALL(++o->refs, 1); case 2: RET_STDCALL(obj_release(o), 1);
    case 3: { uint32_t c = ARG(1); if (!c || G32(c) < 20) RET_STDCALL(DIERR_INVALIDPARAM, 2);
        uint32_t size = G32(c); memset(GPTR(c + 4), 0, size - 4);
        if (o->kind == DEV_JOYSTICK) {
            S32(c + 4, (hostgc_connected() ? 1u : 0u) | 2u); /* attached + polled */
            S32(c + 8, 0x00010115u); S32(c + 12, JOY_AXIS_COUNT); S32(c + 16, HOSTGC_BUTTON_COUNT);
            if (size >= 24) S32(c + 20, 1);
        } else {
            S32(c + 4, 1); S32(c + 8, o->kind == DEV_KEYBOARD ? 0x113 : 0x112);
            S32(c + 12, o->kind == DEV_MOUSE ? 3 : 0); S32(c + 16, o->kind == DEV_MOUSE ? 8 : 128);
        }
        RET_STDCALL(DI_OK, 2); }
    case 4: { uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
        uint32_t hr = o->kind == DEV_JOYSTICK ? joy_enum_objects(cpu, cb, ref, flags) : DI_OK;
        RET_STDCALL(hr, 4); }
    case 5: { uint32_t hr = joy_get_property(o, ARG(1), ARG(2)); RET_STDCALL(hr, 3); }
    case 6: { uint32_t hr = joy_set_property(o, ARG(1), ARG(2)); RET_STDCALL(hr, 3); }
    case 7: {
        if (o->kind == DEV_JOYSTICK) {
            HostGCSnapshot cur; hostgc_init();
            /* Halo retries Acquire every frame while the pad is away: 1,961
             * identical lines in one Build74 session. Say it once per absence
             * and count the rest for the device report. */
            if (!joy_read_gameplay_snapshot(&cur)) {
                o->acquired = 0; atomic_fetch_add(&diagnostic_acquire_lost, 1);
                if (!o->logged_acquire_failure) { o->logged_acquire_failure = 1; host_log("dinput8 joystick Acquire -> input lost (retries counted, not logged)"); }
                RET_STDCALL(DIERR_INPUTLOST, 1);
            }
            joy_clear_buffer(o); o->last = cur; o->have_last = 1;
            o->logged_poll_failure = 0; o->logged_acquire_failure = 0; host_log("dinput8 joystick Acquire -> acquired");
        }
        if (o->kind == DEV_KEYBOARD) {
            joy_clear_buffer(o);
            host_dinput_keyboard_state(o->key_last);
            o->have_key_last = 1;
        }
        o->acquired = 1; RET_STDCALL(DI_OK, 1); }
    case 8:
        o->acquired = 0; o->have_last = 0; o->have_key_last = 0;
        memset(&o->last, 0, sizeof o->last); memset(o->key_last, 0, sizeof o->key_last); joy_clear_buffer(o);
        RET_STDCALL(DI_OK, 1);
    case 9: { uint32_t n = ARG(1), buf = ARG(2);
        if (!buf || !n) RET_STDCALL(DIERR_INVALIDPARAM, 3);
        memset(GPTR(buf), 0, n);
        if (!o->acquired) RET_STDCALL(DIERR_NOTACQUIRED, 3);
        if (o->kind == DEV_KEYBOARD) {
            uint8_t keys[256]; host_dinput_keyboard_state(keys);
            memcpy(GPTR(buf), keys, n < 256 ? n : 256);
        } else if (o->kind == DEV_MOUSE) {
            if (n >= 12) { S32(buf, (uint32_t)host_mouse_dx); S32(buf + 4, (uint32_t)host_mouse_dy); S32(buf + 8, (uint32_t)host_mouse_dz); }
            if (n > 12) { uint8_t merged[8]; host_dinput_mouse_buttons_state(merged); uint32_t buttons = n - 12 < 8 ? n - 12 : 8; memcpy(GPTR(buf + 12), merged, buttons); }
            host_mouse_dx = host_mouse_dy = host_mouse_dz = 0;
        } else {
            HostGCSnapshot cur; uint32_t hr = joy_poll_acquired(o, &cur);
            if (hr != DI_OK) RET_STDCALL(hr, 3);
            joy_fill_state(o, &cur, buf, n);
            atomic_fetch_add(&diagnostic_gamepad_reads,1);
        }
        RET_STDCALL(DI_OK, 3); }
    case 10: {
        uint32_t hr;
        if (o->kind == DEV_JOYSTICK) hr = joy_get_data(o, ARG(1), ARG(2), ARG(3), ARG(4));
        else if (o->kind == DEV_KEYBOARD) hr = keyboard_get_data(o, ARG(1), ARG(2), ARG(3), ARG(4));
        else { uint32_t inout = ARG(3); if (inout) S32(inout, 0); hr = o->acquired ? DI_OK : DIERR_NOTACQUIRED; }
        RET_STDCALL(hr, 5); }
    case 11: {
        uint32_t hr = DI_OK;
        if (o->kind == DEV_JOYSTICK && !joy_parse_format(o, ARG(1))) hr = DIERR_INVALIDPARAM;
        RET_STDCALL(hr, 2); }
    case 12: case 13: RET_STDCALL(DI_OK, m_dev[i].argc);
    case 14: { uint32_t info = ARG(1); if (!info || G32(info) < 32) RET_STDCALL(DIERR_INVALIDPARAM, 4);
        uint32_t size = G32(info); memset(GPTR(info + 4), 0, size - 4);
        snprintf((char *)GPTR(info + 32), size > 32 ? size - 32 : 0, "%s", o->kind == DEV_JOYSTICK ? "Gamepad control" : "Key");
        RET_STDCALL(DI_OK, 4); }
    case 15: { uint32_t info = ARG(1); if (!info || G32(info) < 40) RET_STDCALL(DIERR_INVALIDPARAM, 2);
        uint32_t size = G32(info); memset(GPTR(info + 4), 0, size - 4);
        const char *name = o->kind == DEV_KEYBOARD ? "Keyboard" : o->kind == DEV_MOUSE ? "Mouse" : "Halo Vision Gamepad";
        uint32_t type = o->kind == DEV_KEYBOARD ? 0x113 : o->kind == DEV_MOUSE ? 0x112 : 0x00010115u;
        if (o->kind == DEV_JOYSTICK) { S32(info + 4, GUID_Joystick_D1); S32(info + 20, GUID_Joystick_D1); }
        S32(info + 36, type);
        if (size > 40) snprintf((char *)GPTR(info + 40), size - 40 < 260 ? size - 40 : 260, "%s", name);
        if (size > 300) snprintf((char *)GPTR(info + 300), size - 300 < 260 ? size - 300 : 260, "%s", name);
        RET_STDCALL(DI_OK, 2); }
    case 16: case 17: RET_STDCALL(DI_OK, m_dev[i].argc);
    case 18: S32(ARG(3), 0); RET_STDCALL(0x80004001u, 5);
    case 19: case 20: RET_STDCALL(DI_OK, m_dev[i].argc);
    case 21: S32(ARG(1), 0); RET_STDCALL(DI_OK, 2);
    case 22: case 23: case 24: RET_STDCALL(DI_OK, m_dev[i].argc);
    case 25: {
        if (!o->acquired) {
            if (o->kind == DEV_JOYSTICK && !o->logged_poll_failure++) host_log("dinput8 joystick Poll -> not acquired");
            RET_STDCALL(DIERR_NOTACQUIRED, 1);
        }
        if (o->kind == DEV_JOYSTICK) { uint32_t hr = joy_poll_acquired(o, NULL);
            if (hr == DI_OK) o->logged_poll_failure = 0;
            else if (!o->logged_poll_failure++) host_log("dinput8 joystick Poll -> %08X", hr);
            RET_STDCALL(hr, 1); }
        RET_STDCALL(DI_OK, 1); }
    default: RET_STDCALL(0x80004001u, m_dev[i].argc);
    }
}
#define METHOD_SHIM(cls, idx, handler) static void shim_##cls##_##idx(EngineCPU *cpu) { DIObj *o = obj_from_guest(ARG(0)); if (!o) { host_log("dinput8 method on bad object %08X", ARG(0)); engine_fail(cpu, "dinput8 bad this"); } handler(cpu, o, idx); }
METHOD_SHIM(di,0,method_di) METHOD_SHIM(di,1,method_di) METHOD_SHIM(di,2,method_di) METHOD_SHIM(di,3,method_di) METHOD_SHIM(di,4,method_di) METHOD_SHIM(di,5,method_di) METHOD_SHIM(di,6,method_di) METHOD_SHIM(di,7,method_di) METHOD_SHIM(di,8,method_di) METHOD_SHIM(di,9,method_di) METHOD_SHIM(di,10,method_di)
METHOD_SHIM(dev,0,method_dev) METHOD_SHIM(dev,1,method_dev) METHOD_SHIM(dev,2,method_dev) METHOD_SHIM(dev,3,method_dev) METHOD_SHIM(dev,4,method_dev) METHOD_SHIM(dev,5,method_dev) METHOD_SHIM(dev,6,method_dev) METHOD_SHIM(dev,7,method_dev) METHOD_SHIM(dev,8,method_dev) METHOD_SHIM(dev,9,method_dev) METHOD_SHIM(dev,10,method_dev) METHOD_SHIM(dev,11,method_dev) METHOD_SHIM(dev,12,method_dev) METHOD_SHIM(dev,13,method_dev) METHOD_SHIM(dev,14,method_dev) METHOD_SHIM(dev,15,method_dev) METHOD_SHIM(dev,16,method_dev) METHOD_SHIM(dev,17,method_dev) METHOD_SHIM(dev,18,method_dev) METHOD_SHIM(dev,19,method_dev) METHOD_SHIM(dev,20,method_dev) METHOD_SHIM(dev,21,method_dev) METHOD_SHIM(dev,22,method_dev) METHOD_SHIM(dev,23,method_dev) METHOD_SHIM(dev,24,method_dev) METHOD_SHIM(dev,25,method_dev) METHOD_SHIM(dev,26,method_dev) METHOD_SHIM(dev,27,method_dev) METHOD_SHIM(dev,28,method_dev) METHOD_SHIM(dev,29,method_dev) METHOD_SHIM(dev,30,method_dev) METHOD_SHIM(dev,31,method_dev)
static HostShim di_fns[] = { shim_di_0,shim_di_1,shim_di_2,shim_di_3,shim_di_4,shim_di_5,shim_di_6,shim_di_7,shim_di_8,shim_di_9,shim_di_10 };
static HostShim dev_fns[] = { shim_dev_0,shim_dev_1,shim_dev_2,shim_dev_3,shim_dev_4,shim_dev_5,shim_dev_6,shim_dev_7,shim_dev_8,shim_dev_9,shim_dev_10,shim_dev_11,shim_dev_12,shim_dev_13,shim_dev_14,shim_dev_15,shim_dev_16,shim_dev_17,shim_dev_18,shim_dev_19,shim_dev_20,shim_dev_21,shim_dev_22,shim_dev_23,shim_dev_24,shim_dev_25,shim_dev_26,shim_dev_27,shim_dev_28,shim_dev_29,shim_dev_30,shim_dev_31 };

SHIM(DirectInput8Create) { if (!vt_di) { vt_di = make_vtable("IDirectInput8A", m_di, N(m_di)); vt_dev = make_vtable("IDirectInputDevice8A", m_dev, N(m_dev)); }
    uint32_t out = ARG(3); if (!out) RET_STDCALL(DIERR_INVALIDPARAM, 5); S32(out, 0);
    DIObj *o = obj_new(0, vt_di); if (!o) RET_STDCALL(DIERR_OUTOFMEMORY, 5); S32(out, o->guest); host_log("DirectInput8Create(version %04X) -> input object", ARG(1)); RET_STDCALL(DI_OK, 5); }

static HostShimEntry table[64]; static char names[64][96];
const HostShimEntry *host_shims_dinput8_build(void) {
    int n = 0; table[n++] = (HostShimEntry){ "dinput8.dll", "DirectInput8Create", shim_DirectInput8Create };
    for (int i = 0; i < N(m_di); i++) { snprintf(names[n], 96, "IDirectInput8A::%s", m_di[i].name); table[n] = (HostShimEntry){ "dinput8.dll", names[n], di_fns[i] }; n++; }
    for (int i = 0; i < N(m_dev); i++) { snprintf(names[n], 96, "IDirectInputDevice8A::%s", m_dev[i].name); table[n] = (HostShimEntry){ "dinput8.dll", names[n], dev_fns[i] }; n++; }
    table[n] = (HostShimEntry){ NULL, NULL, NULL }; return table;
}
