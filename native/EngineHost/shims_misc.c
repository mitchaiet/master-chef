/* USER32 / GDI32 / ADVAPI32 / OLE32 / WINMM / VERSION / WINSOCK / dynamically loaded DLL stand-ins. */
#include "host.h"
#include "directsound.h"
#include "vorbis_shim.h"
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <CommonCrypto/CommonDigest.h>

int host_quit_requested;
static uint32_t screen_w = 640, screen_h = 480;
/* The virtual adapter is 1920x1080 (D3D9 GetAdapterDisplayMode). Window
 * dimensions must not replace desktop dimensions: Halo checks that its
 * requested window fits the desktop and otherwise falls back to 800x600. */
/* The engine only accepts a video mode strictly smaller than the desktop, so the
 * virtual desktop must exceed the largest mode in the d3d9 table (2560x1920). */
enum { HOST_DESKTOP_HWND = 0x10000, HOST_DESKTOP_WIDTH = 4096, HOST_DESKTOP_HEIGHT = 3072 };
enum { MAX_WINDOW_CLASSES = 16 };
typedef struct { char name[128]; uint32_t atom, wndproc; } HostWindowClass;
static HostWindowClass window_classes[MAX_WINDOW_CLASSES];
static int window_class_count;
static uint32_t main_wndproc;
static int main_window_active;

/* The original text editor consumes ordinary Windows key messages separately
 * from DirectInput. Queue on the engine's UI thread before its message pump,
 * so a press cannot arrive one frame late and accept a newly opened dialog.
 * Return/Escape serve text dialogs. Arrow navigation stays in DirectInput;
 * also posting arrow messages causes duplicate selection steps in submenus. */
typedef struct { uint32_t hwnd, message, wparam, lparam, time, x, y; } HostMessage;
enum { MAX_HOST_MESSAGES = 256 };
static _Thread_local HostMessage host_messages[MAX_HOST_MESSAGES];
static _Thread_local unsigned host_message_count;
static _Thread_local uint8_t message_keyboard_last[256];
static void queue_message(uint32_t hwnd, uint32_t message, uint32_t wp, uint32_t lp) {
    if (host_message_count == MAX_HOST_MESSAGES) {
        host_log("USER32 input queue full; dropping oldest message");
        memmove(host_messages, host_messages + 1, sizeof(HostMessage) * (--host_message_count));
    }
    host_messages[host_message_count++] = (HostMessage){hwnd, message, wp, lp,
        (uint32_t)(host_monotonic_ns() / 1000000ull), 0, 0};
}
static void pump_keyboard_messages(void) {
    if (!main_window_active || !main_wndproc) return;
    uint8_t keys[256]; host_dinput_keyboard_state(keys);
    static const uint8_t scan[] = {0x01, 0x1C};
    static const uint8_t vk[] = {0x1B, 0x0D};
    for (unsigned i = 0; i < sizeof scan; i++) {
        unsigned dik = scan[i]; int down = (keys[dik] & 0x80) != 0;
        if (down == ((message_keyboard_last[dik] & 0x80) != 0)) continue;
        uint32_t lp = 1u | ((dik & 0x7Fu) << 16) | ((dik & 0x80u) ? 1u << 24 : 0);
        if (!down) lp |= 0xC0000000u;
        queue_message(host_main_hwnd, down ? 0x100 : 0x101, vk[i], lp);
    }
    memcpy(message_keyboard_last, keys, sizeof keys);
}


static uint32_t call_wndproc(EngineCPU *cpu, uint32_t proc, uint32_t hwnd,
                             uint32_t message, uint32_t wparam, uint32_t lparam) {
    if (!proc) return 0;
    uint32_t args[4] = { hwnd, message, wparam, lparam };
    return host_call_guest(cpu, proc, 4, args, 1);
}

/* Minimal wsprintfA-style formatter reading varargs from the guest stack (cdecl). */
static uint32_t guest_format(EngineCPU *cpu, uint32_t out, const char *fmt, uint32_t argbase) {
    char *o = (char *)GPTR(out); size_t n = 0; uint32_t ai = 0;
    while (*fmt && n < 1000) {
        if (*fmt != '%') { o[n++] = *fmt++; continue; }
        fmt++; if (*fmt == '%') { o[n++] = '%'; fmt++; continue; }
        char spec[16]; size_t si = 0; spec[si++] = '%';
        while (*fmt && strchr("-+ #0123456789.lh", *fmt) && si < 14) { if (*fmt != 'l' && *fmt != 'h') spec[si++] = *fmt; fmt++; }
        char conv = *fmt++; spec[si++] = conv; spec[si] = 0;
        uint32_t v = G32(argbase + 4 * ai++);
        char tmp[512];
        if (conv == 's') snprintf(tmp, sizeof tmp, spec, v ? GSTR(v) : "(null)");
        else if (conv == 'c') snprintf(tmp, sizeof tmp, spec, (int)v);
        else if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, (int)v);
        else snprintf(tmp, sizeof tmp, spec, v);
        size_t t = strlen(tmp); memcpy(o + n, tmp, t); n += t;
    }
    o[n] = 0; return (uint32_t)n;
}

/* ---- USER32 ---- */
SHIM(RegisterClassExA) { uint32_t wc = ARG(0);
    if (!wc || G32(wc) < 48 || !G32(wc + 8) || !G32(wc + 40) || window_class_count >= MAX_WINDOW_CLASSES) RET_STDCALL(0, 1);
    HostWindowClass *entry = &window_classes[window_class_count++]; memset(entry, 0, sizeof *entry);
    snprintf(entry->name, sizeof entry->name, "%s", GSTR(G32(wc + 40)));
    entry->wndproc = G32(wc + 8); entry->atom = 0xC001u + (uint32_t)window_class_count - 1u;
    host_log("RegisterClassExA(%s) -> atom %04X wndproc %08X", entry->name, entry->atom, entry->wndproc);
    RET_STDCALL(entry->atom, 1); }
SHIM(UnregisterClassA) { uint32_t name = ARG(0);
    for (int i = 0; i < window_class_count; i++) {
        int match = name <= 0xFFFFu ? window_classes[i].atom == name : !strcmp(window_classes[i].name, GSTR(name));
        if (match) { window_classes[i] = window_classes[--window_class_count]; RET_STDCALL(1, 2); }
    }
    RET_STDCALL(0, 2); }
SHIM(CreateWindowExA) { uint32_t class_id = ARG(1); const char *class_name = class_id > 0xFFFFu ? GSTR(class_id) : NULL;
    host_log("CreateWindowExA(class %s, title \"%s\", style %08X, %d x %d)", class_name ? class_name : "<atom>", ARG(2) ? GSTR(ARG(2)) : "", ARG(3), (int)ARG(6), (int)ARG(7));
    /* host_main_hwnd is a reusable guest token, not proof that two successive
     * CreateWindowEx calls describe the same Win32 window. Halo creates a
     * temporary detection window before its game window. */
    if (main_window_active && main_wndproc) {
        call_wndproc(cpu, main_wndproc, host_main_hwnd, 0x0008, 0, 0);
        call_wndproc(cpu, main_wndproc, host_main_hwnd, 0x001C, 0, 0);
    }
    main_window_active = 0;
    main_wndproc = 0;
    host_message_count = 0; memset(message_keyboard_last, 0, sizeof message_keyboard_last);
    for (int i = 0; i < window_class_count; i++)
        if (class_name ? !strcmp(window_classes[i].name, class_name) : window_classes[i].atom == class_id) { main_wndproc = window_classes[i].wndproc; break; }
    if ((int)ARG(6) > 0 && ARG(6) < 8192) screen_w = ARG(6); if ((int)ARG(7) > 0 && ARG(7) < 8192) screen_h = ARG(7);
    /* CreateWindowEx dispatches WM_CREATE synchronously before returning. */
    host_log("USER32 lifecycle: WM_CREATE hwnd %08X wndproc %08X", host_main_hwnd, main_wndproc);
    call_wndproc(cpu, main_wndproc, host_main_hwnd, 0x0001, 0, 0);
    RET_STDCALL(host_main_hwnd, 12); }
SHIM(DestroyWindow) { uint32_t hwnd = ARG(0);
    if (main_window_active) {
        host_log("USER32 lifecycle: deactivate/destroy hwnd %08X wndproc %08X", hwnd, main_wndproc);
        call_wndproc(cpu, main_wndproc, hwnd, 0x0008, 0, 0);
        call_wndproc(cpu, main_wndproc, hwnd, 0x001C, 0, 0);
    }
    call_wndproc(cpu, main_wndproc, hwnd, 0x0002, 0, 0); /* WM_DESTROY */
    main_window_active = 0; main_wndproc = 0; RET_STDCALL(1, 1); }
SHIM(ShowWindow) { int show = ARG(1) != 0, was_visible = main_window_active;
    if (show) {
        call_wndproc(cpu, main_wndproc, ARG(0), 0x0018, 1, 0); /* WM_SHOWWINDOW */
        if (!main_window_active) {
            host_log("USER32 lifecycle: activate/show hwnd %08X wndproc %08X", ARG(0), main_wndproc);
            call_wndproc(cpu, main_wndproc, ARG(0), 0x001C, 1, 0); /* WM_ACTIVATEAPP */
            call_wndproc(cpu, main_wndproc, ARG(0), 0x0006, 1, 0); /* WM_ACTIVATE / WA_ACTIVE */
            call_wndproc(cpu, main_wndproc, ARG(0), 0x0007, 0, 0); /* WM_SETFOCUS */
            main_window_active = 1;
        }
    } else if (main_window_active) {
        call_wndproc(cpu, main_wndproc, ARG(0), 0x0008, 0, 0); /* WM_KILLFOCUS */
        call_wndproc(cpu, main_wndproc, ARG(0), 0x0006, 0, 0); /* WM_ACTIVATE / WA_INACTIVE */
        call_wndproc(cpu, main_wndproc, ARG(0), 0x001C, 0, 0); /* WM_ACTIVATEAPP */
        call_wndproc(cpu, main_wndproc, ARG(0), 0x0018, 0, 0); /* WM_SHOWWINDOW */
        main_window_active = 0;
    }
    /* Win32 returns the previous visibility state. */
    RET_STDCALL(was_visible, 2); }
SHIM(SetWindowPos) { RET_STDCALL(1, 7); }
SHIM(MoveWindow) { RET_STDCALL(1, 6); }
static void window_rect(uint32_t hwnd, uint32_t r) {
    S32(r, 0); S32(r + 4, 0);
    S32(r + 8, hwnd == HOST_DESKTOP_HWND ? HOST_DESKTOP_WIDTH : screen_w);
    S32(r + 12, hwnd == HOST_DESKTOP_HWND ? HOST_DESKTOP_HEIGHT : screen_h);
}
SHIM(GetClientRect) { window_rect(ARG(0), ARG(1)); RET_STDCALL(1, 2); }
SHIM(GetWindowRect) { window_rect(ARG(0), ARG(1)); RET_STDCALL(1, 2); }
SHIM(AdjustWindowRect) { RET_STDCALL(1, 3); }
SHIM(GetWindowPlacement) { uint32_t p = ARG(1); memset(GPTR(p), 0, 44); S32(p, 44); S32(p + 8, 1); S32(p + 36, screen_w); S32(p + 40, screen_h); RET_STDCALL(1, 2); }
SHIM(PeekMessageA) {
    uint32_t out = ARG(0), hwnd = ARG(1), low = ARG(2), high = ARG(3), flags = ARG(4);
    if (!out) RET_STDCALL(0, 5);
    pump_keyboard_messages();
    for (unsigned i = 0; i < host_message_count; i++) {
        HostMessage msg = host_messages[i];
        if (hwnd && msg.hwnd != hwnd) continue;
        if ((low || high) && (msg.message < low || msg.message > high)) continue;
        memcpy(GPTR(out), &msg, sizeof msg);
        if (flags & 1u) {
            memmove(host_messages + i, host_messages + i + 1,
                    sizeof(HostMessage) * (host_message_count - i - 1));
            host_message_count--;
        }
        RET_STDCALL(1, 5);
    }
    RET_STDCALL(0, 5);
}
SHIM(TranslateMessage) {
    uint32_t msg = ARG(0); if (!msg) RET_STDCALL(0, 1);
    uint32_t type = G32(msg + 4), vk = G32(msg + 8);
    if (type != 0x100 && type != 0x101 && type != 0x104 && type != 0x105) RET_STDCALL(0, 1);
    if ((type == 0x100 || type == 0x104) && (vk == 0x0D || vk == 0x1B))
        queue_message(G32(msg), type == 0x100 ? 0x102 : 0x106, vk, G32(msg + 12));
    RET_STDCALL(1, 1);
}
SHIM(DispatchMessageA) { uint32_t msg = ARG(0); RET_STDCALL(msg ? call_wndproc(cpu, main_wndproc, G32(msg), G32(msg + 4), G32(msg + 8), G32(msg + 12)) : 0, 1); }
SHIM(DefWindowProcA) { RET_STDCALL(0, 4); }
SHIM(CallWindowProcA) { RET_STDCALL(call_wndproc(cpu, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)), 5); }
SHIM(PostQuitMessage) { host_quit_requested = 1; RET_STDCALL(0, 1); }
SHIM(SendMessageA) { RET_STDCALL(call_wndproc(cpu, main_wndproc, ARG(0), ARG(1), ARG(2), ARG(3)), 4); }
SHIM(MsgWaitForMultipleObjects) { RET_STDCALL(0, 5); }
SHIM(GetActiveWindow) { RET_STDCALL(host_main_hwnd, 0); }
SHIM(GetForegroundWindow) { RET_STDCALL(host_main_hwnd, 0); }
SHIM(SetActiveWindow) { RET_STDCALL(host_main_hwnd, 1); }
SHIM(SetForegroundWindow) { RET_STDCALL(1, 1); }
SHIM(SetFocus) { RET_STDCALL(host_main_hwnd, 1); }
SHIM(GetDesktopWindow) { RET_STDCALL(HOST_DESKTOP_HWND, 0); }
SHIM(GetParent) { RET_STDCALL(0, 1); }
SHIM(FindWindowA) { RET_STDCALL(0, 2); }
SHIM(EnableWindow) { RET_STDCALL(0, 2); }
SHIM(GetSystemMetrics) { uint32_t i = ARG(0); RET_STDCALL(i == 0 ? HOST_DESKTOP_WIDTH : i == 1 ? HOST_DESKTOP_HEIGHT : i == 80 ? 1 : 0, 1); }
SHIM(GetDC) { RET_STDCALL(0x20001, 1); }
SHIM(ReleaseDC) { RET_STDCALL(1, 2); }
SHIM(ShowCursor) { RET_STDCALL(0, 1); }
SHIM(SetCursor) { RET_STDCALL(0, 1); }
SHIM(LoadCursorA) { RET_STDCALL(0x30001, 2); }
SHIM(LoadIconA) { RET_STDCALL(0x30002, 2); }
SHIM(LoadBitmapA) { RET_STDCALL(0, 2); }
SHIM(GetCursorPos) { S32(ARG(0), screen_w / 2); S32(ARG(0) + 4, screen_h / 2); RET_STDCALL(1, 1); }
SHIM(ClientToScreen) { RET_STDCALL(1, 2); }
SHIM(SetCapture) { RET_STDCALL(0, 1); }
SHIM(ReleaseCapture) { RET_STDCALL(1, 0); }
SHIM(GetCapture) { RET_STDCALL(0, 0); }
SHIM(GetAsyncKeyState) { RET_STDCALL(0, 1); }
SHIM(GetKeyState) { RET_STDCALL(0, 1); }
SHIM(GetDoubleClickTime) { RET_STDCALL(500, 0); }
SHIM(MessageBoxA) { host_log("MessageBoxA: [%s] %s", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : ""); RET_STDCALL(1, 4); }
SHIM(SetWindowTextA) { RET_STDCALL(1, 2); }
SHIM(GetWindowLongA) { RET_STDCALL(0, 2); }
SHIM(SetWindowLongA) { RET_STDCALL(0, 3); }
SHIM(InvalidateRect) { RET_STDCALL(1, 3); }
SHIM(ValidateRect) { RET_STDCALL(1, 2); }
SHIM(PtInRect) { uint32_t r = ARG(0); int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    RET_STDCALL(x >= (int32_t)G32(r) && x < (int32_t)G32(r + 8) && y >= (int32_t)G32(r + 4) && y < (int32_t)G32(r + 12), 3); }
SHIM(wsprintfA) { uint32_t out = ARG(0); uint32_t n = guest_format(cpu, out, GSTR(ARG(1)), cpu->gpr[4] + 12); RET_CDECL(n); }
SHIM(IsClipboardFormatAvailable) { RET_STDCALL(0, 1); }
SHIM(OpenClipboard) { RET_STDCALL(0, 1); }
SHIM(CloseClipboard) { RET_STDCALL(1, 0); }
SHIM(GetClipboardData) { RET_STDCALL(0, 1); }
SHIM(SetPropA) { RET_STDCALL(1, 3); }
SHIM(GetPropA) { RET_STDCALL(0, 2); }
SHIM(RemovePropA) { RET_STDCALL(0, 2); }
uint32_t host_dialog_choose(uint32_t hinst, uint32_t id, char *summary, size_t cap);
extern uint32_t host_call_guest(EngineCPU *cpu, uint32_t fn, int nargs, const uint32_t *args, int callee_pops);
/* DialogBoxParamA must return the value the original dialog procedure passes to
 * EndDialog, not the pressed control id. Halo's adapter/exit warning (template
 * 102) ends with 0 for "Continue Anyway", 1 for "Continue in Safe Mode" and 2
 * for "Exit"; returning the control id (1004) used to be read as nonzero and
 * silently put the whole renderer into Safe Mode (fixed-function, no pixel
 * shaders, lowest settings). Run only the WM_COMMAND handler for the chosen
 * button: it touches IsDlgButtonChecked/EndDialog, both shimmed here. */
static uint32_t dialog_end_result; static int dialog_ended;
SHIM(DialogBoxParamA) { char summary[1024]; uint32_t choice = host_dialog_choose(ARG(0), ARG(1), summary, sizeof summary);
    uint32_t proc = ARG(3), result = choice; const char *source = "control id";
    dialog_ended = 0; dialog_end_result = 0;
    if (proc && choice && choice != 0xFFFFFFFFu) {
        uint32_t command[4] = { 0x00010002u /* dialog handle */, 0x111u /* WM_COMMAND */, choice & 0xFFFFu /* BN_CLICKED, id */, 0 };
        host_call_guest(cpu, proc, 4, command, 1);
        if (dialog_ended) { result = dialog_end_result; source = "EndDialog"; }
        else source = "control id (procedure did not end the dialog)";
    }
    host_log("DialogBoxParamA(template %u): %s  -> pressing control %u, result %u from %s", ARG(1), summary, choice, result, source); RET_STDCALL(result, 5); }
SHIM(DialogBoxIndirectParamA) { host_log("DialogBoxIndirectParamA -> cancelled"); RET_STDCALL(0xFFFFFFFFu, 5); }
SHIM(CreateDialogIndirectParamA) { RET_STDCALL(0, 5); }
SHIM(EndDialog) { dialog_ended = 1; dialog_end_result = ARG(1); RET_STDCALL(1, 2); }
SHIM(GetDlgItem) { RET_STDCALL(0, 2); }
SHIM(SetDlgItemTextA) { RET_STDCALL(1, 3); }
SHIM(IsDlgButtonChecked) { RET_STDCALL(0, 2); }
uint32_t host_load_string(uint32_t hinst, uint32_t id, char *out, uint32_t cap);
SHIM(LoadStringA) { uint32_t buf = ARG(2), cap = ARG(3); char text[2048]; uint32_t n = host_load_string(ARG(0), ARG(1), text, sizeof text);
    if (n) host_trace("[res] LoadStringA(%u) = \"%s\"", ARG(1), text); else host_trace("[res] LoadStringA(%u) -> not found", ARG(1));
    if (buf && cap) { if (n >= cap) n = cap - 1; memcpy(GPTR(buf), text, n); S8(buf + n, 0); } RET_STDCALL(n, 4); }

/* ---- GDI32 ---- */
SHIM(GetDeviceCaps) { uint32_t i = ARG(1); uint32_t v = i == 8 ? HOST_DESKTOP_WIDTH : i == 10 ? HOST_DESKTOP_HEIGHT : i == 12 ? 32 : i == 14 ? 1 : i == 116 ? 60 : (i == 88 || i == 90) ? 96 : 0; RET_STDCALL(v, 2); }
SHIM(GetDeviceGammaRamp) { uint32_t r = ARG(1); for (uint32_t i = 0; i < 256; i++) { uint16_t v = (uint16_t)(i * 257); S16(r + 2 * i, v); S16(r + 512 + 2 * i, v); S16(r + 1024 + 2 * i, v); } RET_STDCALL(1, 2); }
SHIM(SetDeviceGammaRamp) { RET_STDCALL(1, 2); }
SHIM(CreateCompatibleDC) { RET_STDCALL(0x20002, 1); }
SHIM(CreateFontIndirectA) { RET_STDCALL(0x40001, 1); }
SHIM(DeleteObject) { RET_STDCALL(1, 1); }
SHIM(SelectObject) { RET_STDCALL(0, 2); }
SHIM(SetTextColor) { RET_STDCALL(0, 2); }
SHIM(StretchBlt) { RET_STDCALL(1, 11); }
SHIM(GetObjectA) { RET_STDCALL(0, 3); }

/* ---- ADVAPI32: a small in-memory registry seeded from <game root>/halo-vision-registry.txt ---- */
typedef struct { char root[8]; char key[256]; char name[64]; uint32_t type; uint8_t data[512]; uint32_t size; } RegValue;
static RegValue reg_values[256]; static int reg_count, reg_loaded;
typedef struct { char root[8]; char key[256]; } RegKey;
static void reg_path(char *out, size_t cap, const char *root, const char *key) { snprintf(out, cap, "%s|%s", root, key); }
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; }
static void reg_load(void) {
    if (reg_loaded) return; reg_loaded = 1; char path[1024]; snprintf(path, sizeof path, "%s/halo-vision-registry.txt", host_game_root);
    FILE *f = fopen(path, "r"); if (!f) { host_log("no registry seed at %s", path); return; }
    char line[2048];
    while (fgets(line, sizeof line, f) && reg_count < 256) {
        char *fields[5]; int n = 0; char *p = line; line[strcspn(line, "\r\n")] = 0;
        while (n < 5 && p) { fields[n++] = p; char *bar = n < 5 ? strchr(p, '|') : NULL; if (bar) { *bar = 0; p = bar + 1; } else p = NULL; }
        if (n < 5) continue;
        RegValue *v = &reg_values[reg_count++]; memset(v, 0, sizeof *v);
        snprintf(v->root, sizeof v->root, "%s", fields[0]); snprintf(v->key, sizeof v->key, "%s", fields[1]); snprintf(v->name, sizeof v->name, "%s", fields[2]); v->type = (uint32_t)atoi(fields[3]);
        if (v->type == 4) { uint32_t d = (uint32_t)strtoul(fields[4], NULL, 10); memcpy(v->data, &d, 4); v->size = 4; }
        else if (v->type == 3) { size_t len = strlen(fields[4]) / 2; if (len > sizeof v->data) len = sizeof v->data; for (size_t i = 0; i < len; i++) v->data[i] = (uint8_t)(hexval(fields[4][2 * i]) * 16 + hexval(fields[4][2 * i + 1])); v->size = (uint32_t)len; }
        else { snprintf((char *)v->data, sizeof v->data, "%s", fields[4]); v->size = (uint32_t)strlen((char *)v->data) + 1; }
    }
    fclose(f); host_log("registry seed loaded: %d values", reg_count);
}
static void reg_save(void) {
    char path[1024]; snprintf(path, sizeof path, "%s/halo-vision-registry.txt", host_game_root); FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < reg_count; i++) { RegValue *v = &reg_values[i]; fprintf(f, "%s|%s|%s|%u|", v->root, v->key, v->name, v->type);
        if (v->type == 4) { uint32_t d; memcpy(&d, v->data, 4); fprintf(f, "%u\n", d); } else if (v->type == 3) { for (uint32_t k = 0; k < v->size; k++) fprintf(f, "%02x", v->data[k]); fputc('\n', f); } else fprintf(f, "%s\n", (char *)v->data); }
    fclose(f);
}
static const char *root_name(uint32_t h) { return h == 0x80000001u ? "HKCU" : h == 0x80000002u ? "HKLM" : h == 0x80000000u ? "HKCR" : NULL; }
static RegKey *key_from_handle(uint32_t h) { return host_handle_object(h, HANDLE_MISC); }
static int resolve_key(uint32_t h, const char *sub, char *root, char *key) {
    const char *rn = root_name(h); RegKey *k = rn ? NULL : key_from_handle(h);
    if (!rn && !k) return 0;
    strcpy(root, rn ? rn : k->root);
    if (rn) snprintf(key, 256, "%s", sub ? sub : ""); else if (sub && *sub) snprintf(key, 256, "%s\\%s", k->key, sub); else snprintf(key, 256, "%s", k->key);
    return 1;
}
static int key_exists(const char *root, const char *key) { reg_load(); size_t n = strlen(key); for (int i = 0; i < reg_count; i++) if (!strcasecmp(reg_values[i].root, root) && !strncasecmp(reg_values[i].key, key, n) && (reg_values[i].key[n] == 0 || reg_values[i].key[n] == '\\')) return 1; return 0; }
static uint32_t open_key(const char *root, const char *key) { RegKey *k = calloc(1, sizeof *k); strcpy(k->root, root); snprintf(k->key, sizeof k->key, "%s", key); return host_handle_new(HANDLE_MISC, k); }
SHIM(RegOpenKeyExA) { char root[8], key[256]; const char *sub = ARG(1) ? GSTR(ARG(1)) : "";
    if (!resolve_key(ARG(0), sub, root, key) || !key_exists(root, key)) { host_trace("[reg] RegOpenKeyExA(%s) -> not found", sub); RET_STDCALL(2, 5); }
    S32(ARG(4), open_key(root, key)); host_trace("[reg] RegOpenKeyExA(%s\\%s) -> ok", root, key); RET_STDCALL(0, 5); }
SHIM(RegOpenKeyA) { char root[8], key[256]; const char *sub = ARG(1) ? GSTR(ARG(1)) : "";
    if (!resolve_key(ARG(0), sub, root, key) || !key_exists(root, key)) { host_trace("[reg] RegOpenKeyA(%s) -> not found", sub); RET_STDCALL(2, 3); }
    S32(ARG(2), open_key(root, key)); RET_STDCALL(0, 3); }
SHIM(RegCreateKeyExA) { char root[8], key[256]; const char *sub = ARG(1) ? GSTR(ARG(1)) : "";
    if (!resolve_key(ARG(0), sub, root, key)) RET_STDCALL(6, 9);
    int existed = key_exists(root, key); if (ARG(7)) S32(ARG(7), open_key(root, key)); if (ARG(8)) S32(ARG(8), existed ? 2 : 1); host_trace("[reg] RegCreateKeyExA(%s\\%s)", root, key); RET_STDCALL(0, 9); }
SHIM(RegCloseKey) { RegKey *k = key_from_handle(ARG(0)); if (k) { free(k); host_handle_close(ARG(0)); } RET_STDCALL(0, 1); }
SHIM(RegQueryValueExA) { RegKey *k = key_from_handle(ARG(0)); const char *name = ARG(1) ? GSTR(ARG(1)) : ""; uint32_t ptype = ARG(3), pdata = ARG(4), psize = ARG(5);
    reg_load(); if (!k) RET_STDCALL(6, 6);
    for (int i = 0; i < reg_count; i++) { RegValue *v = &reg_values[i];
        if (strcasecmp(v->root, k->root) || strcasecmp(v->key, k->key) || strcasecmp(v->name, name)) continue;
        if (ptype) S32(ptype, v->type);
        uint32_t cap = psize ? G32(psize) : 0; if (psize) S32(psize, v->size);
        if (pdata) { if (cap < v->size) RET_STDCALL(234, 6); memcpy(GPTR(pdata), v->data, v->size); }
        host_trace("[reg] RegQueryValueExA(%s) -> type %u, %u bytes", name, v->type, v->size); RET_STDCALL(0, 6); }
    host_trace("[reg] RegQueryValueExA(%s) -> not found", name); RET_STDCALL(2, 6); }
SHIM(RegSetValueExA) { RegKey *k = key_from_handle(ARG(0)); const char *name = ARG(1) ? GSTR(ARG(1)) : ""; uint32_t type = ARG(3), data = ARG(4), size = ARG(5);
    reg_load(); if (!k) RET_STDCALL(6, 6);
    RegValue *v = NULL; for (int i = 0; i < reg_count; i++) if (!strcasecmp(reg_values[i].root, k->root) && !strcasecmp(reg_values[i].key, k->key) && !strcasecmp(reg_values[i].name, name)) v = &reg_values[i];
    if (!v) { if (reg_count >= 256) RET_STDCALL(8, 6); v = &reg_values[reg_count++]; memset(v, 0, sizeof *v); strcpy(v->root, k->root); snprintf(v->key, sizeof v->key, "%s", k->key); snprintf(v->name, sizeof v->name, "%s", name); }
    v->type = type; if (size > sizeof v->data) size = sizeof v->data; memcpy(v->data, GPTR(data), size); v->size = size; reg_save();
    host_trace("[reg] RegSetValueExA(%s) type %u %u bytes", name, type, size); RET_STDCALL(0, 6); }
typedef struct { uint32_t alg; uint8_t buf[65536]; uint32_t len; } HashObj;
static HashObj hashes[16];
SHIM(CryptAcquireContextA) { if (ARG(0)) S32(ARG(0), 0x77770001u); RET_STDCALL(1, 5); }
SHIM(CryptReleaseContext) { RET_STDCALL(1, 2); }
SHIM(CryptCreateHash) { for (int i = 1; i < 16; i++) if (!hashes[i].alg) { hashes[i].alg = ARG(1); hashes[i].len = 0; if (ARG(4)) S32(ARG(4), 0x77770000u + (uint32_t)i); RET_STDCALL(1, 5); } RET_STDCALL(0, 5); }
static HashObj *hash_from(uint32_t h) { uint32_t i = h - 0x77770000u; return (i >= 1 && i < 16 && hashes[i].alg) ? &hashes[i] : NULL; }
SHIM(CryptHashData) { HashObj *h = hash_from(ARG(0)); uint32_t n = ARG(2); if (!h) RET_STDCALL(0, 4); if (h->len + n > sizeof h->buf) n = (uint32_t)sizeof h->buf - h->len; memcpy(h->buf + h->len, GPTR(ARG(1)), n); h->len += n; RET_STDCALL(1, 4); }
SHIM(CryptGetHashParam) { HashObj *h = hash_from(ARG(0)); uint32_t param = ARG(1), out = ARG(2), plen = ARG(3); if (!h) RET_STDCALL(0, 5);
    if (param == 2) { uint8_t digest[32]; uint32_t dlen = 16;
        if (h->alg == 0x8004) { CC_SHA1(h->buf, h->len, digest); dlen = 20; } else { CC_MD5(h->buf, h->len, digest); dlen = 16; }
        if (out) memcpy(GPTR(out), digest, dlen); if (plen) S32(plen, dlen); host_trace("[crypt] hash alg %04X over %u bytes", h->alg, h->len); RET_STDCALL(1, 5); }
    if (param == 4) { if (out) S32(out, h->alg == 0x8004 ? 20 : 16); if (plen) S32(plen, 4); RET_STDCALL(1, 5); }
    if (param == 1) { if (out) S32(out, h->alg); if (plen) S32(plen, 4); RET_STDCALL(1, 5); }
    RET_STDCALL(0, 5); }
SHIM(CryptDestroyHash) { HashObj *h = hash_from(ARG(0)); if (h) h->alg = 0; RET_STDCALL(1, 1); }
SHIM(OpenProcessToken) { RET_STDCALL(0, 3); }
SHIM(OpenThreadToken) { RET_STDCALL(0, 4); }
SHIM(DuplicateToken) { RET_STDCALL(0, 3); }
SHIM(AllocateAndInitializeSid) { RET_STDCALL(0, 11); }
SHIM(FreeSid) { RET_STDCALL(0, 1); }
SHIM(GetLengthSid) { RET_STDCALL(0, 1); }
SHIM(InitializeAcl) { RET_STDCALL(0, 3); }
SHIM(AddAccessAllowedAce) { RET_STDCALL(0, 4); }
SHIM(InitializeSecurityDescriptor) { RET_STDCALL(0, 2); }
SHIM(SetSecurityDescriptorDacl) { RET_STDCALL(0, 4); }
SHIM(SetSecurityDescriptorGroup) { RET_STDCALL(0, 3); }
SHIM(SetSecurityDescriptorOwner) { RET_STDCALL(0, 3); }
SHIM(IsValidSecurityDescriptor) { RET_STDCALL(0, 1); }
SHIM(AccessCheck) { RET_STDCALL(0, 8); }

/* ---- OLE32 / OLEAUT32 ---- */
SHIM(CoInitialize) { RET_STDCALL(0, 1); }
SHIM(CoUninitialize) { RET_STDCALL(0, 0); }
SHIM(CoCreateInstance) { host_log("CoCreateInstance -> class not registered"); RET_STDCALL(0x80040154u, 5); }
SHIM(CLSIDFromString) { RET_STDCALL(0, 2); }
SHIM(VariantInit) { memset(GPTR(ARG(0)), 0, 16); RET_STDCALL(0, 1); }
SHIM(VariantClear) { memset(GPTR(ARG(0)), 0, 16); RET_STDCALL(0, 1); }
SHIM(StringFromGUID2) { uint32_t out = ARG(1), cap = ARG(2); const char *s = "{00000000-0000-0000-0000-000000000000}"; uint32_t n = 39; if (cap < n) RET_STDCALL(0, 3);
    for (uint32_t i = 0; i < 38; i++) S16(out + 2 * i, (uint16_t)s[i]); S16(out + 76, 0); RET_STDCALL(n, 3); }

/* ---- delay-loaded / dynamically loaded DLLs ---- */
SHIM(timeBeginPeriod) { RET_STDCALL(0, 1); }
SHIM(timeEndPeriod) { RET_STDCALL(0, 1); }
SHIM(GetFileVersionInfoSizeA) { RET_STDCALL(0, 2); }
SHIM(GetFileVersionInfoA) { RET_STDCALL(0, 4); }
SHIM(VerQueryValueA) { RET_STDCALL(0, 4); }
SHIM(ShellExecuteA) { host_log("ShellExecuteA(%s)", ARG(2) ? GSTR(ARG(2)) : ""); RET_STDCALL(42, 6); }
SHIM(InternetQueryOptionA) { RET_STDCALL(0, 4); }
SHIM(DirectSoundCreate8) { host_dsound_create8(cpu); }
SHIM(DirectSoundCreate) { host_dsound_create(cpu); }
SHIM(DirectSoundEnumerateA) { uint32_t cb = ARG(0), ctx = ARG(1); static uint32_t desc, mod; if (!desc) { desc = guest_strdup("Primary Sound Driver"); mod = guest_strdup(""); }
    uint32_t args[4] = { 0, desc, mod, ctx }; host_log("DirectSoundEnumerateA: reporting one playback device"); host_call_guest(cpu, cb, 4, args, 1); RET_STDCALL(0, 2); }
SHIM(DirectSoundCaptureEnumerateA) { host_log("DirectSoundCaptureEnumerateA: no capture devices"); RET_STDCALL(0, 2); }
SHIM(DirectSoundGetDeviceID) { RET_STDCALL(0x88780078u, 2); }
SHIM(EBUEula) { host_log("EBUEula(%s) -> accepted", ARG(0) ? GSTR(ARG(0)) : ""); RET_STDCALL(1, 4); }
SHIM(DisableD3DSpy) { RET_STDCALL(0, 0); }
/* Halo005841BA calls this cdecl export and pops its argument at005841BC. */
SHIM(DebugSetMute) { RET_CDECL(0); }
/* Original00516F25 calls this export, then00516F27 cleans both arguments. */
SHIM(NvCplGetDataInt) { if (ARG(1)) S32(ARG(1), 0); host_trace("[nvcpl] NvCplGetDataInt(%u) -> unavailable", ARG(0)); RET_CDECL(0); }
SHIM(NvCplSetDataInt) { RET_STDCALL(0, 2); }
SHIM(SHGetFolderPathA) { uint32_t csidl = ARG(1) & 0xFF, out = ARG(4); const char *path = csidl == 5 ? "C:\\Users\\Player\\Documents" : csidl == 0x1A ? "C:\\Users\\Player\\AppData\\Roaming" : csidl == 0x26 ? "C:\\Program Files" : "C:\\Users\\Player";
    if (out) strcpy((char *)GPTR(out), path); host_trace("[shell] SHGetFolderPathA(csidl %u) -> %s", csidl, path); RET_STDCALL(0, 5); }
/* Original005196E9 passes seven arguments and cleans them at005196EB. */
SHIM(KeystoneCreate) { host_log("KeystoneCreate -> NULL"); RET_CDECL(0); }
SHIM(BinkOpen) { host_log("BinkOpen(%s) -> NULL (videos skipped)", ARG(0) ? GSTR(ARG(0)) : ""); RET_STDCALL(0, 2); }
SHIM(BinkSetSoundSystem) { RET_STDCALL(0, 2); }
SHIM(BinkOpenDirectSound) { RET_STDCALL(0, 1); }
SHIM(ov_open_callbacks) { host_vorbis_open_callbacks(cpu); }
SHIM(ov_clear) { host_vorbis_clear(cpu); }
SHIM(ov_read) { host_vorbis_read(cpu); }
SHIM(ov_crosslap) { host_vorbis_crosslap(cpu); }

#define U(n) { "USER32.dll", #n, shim_##n }
#define G(n) { "GDI32.dll", #n, shim_##n }
#define A(n) { "ADVAPI32.dll", #n, shim_##n }
#define O(n) { "ole32.dll", #n, shim_##n }
const HostShimEntry host_shims_misc[] = {
    U(RegisterClassExA), U(UnregisterClassA), U(CreateWindowExA), U(DestroyWindow), U(ShowWindow), U(SetWindowPos), U(MoveWindow), U(GetClientRect), U(GetWindowRect), U(AdjustWindowRect), U(GetWindowPlacement),
    U(PeekMessageA), U(TranslateMessage), U(DispatchMessageA), U(DefWindowProcA), U(CallWindowProcA), U(PostQuitMessage), U(SendMessageA), U(MsgWaitForMultipleObjects),
    U(GetActiveWindow), U(GetForegroundWindow), U(SetActiveWindow), U(SetForegroundWindow), U(SetFocus), U(GetDesktopWindow), U(GetParent), U(FindWindowA), U(EnableWindow), U(GetSystemMetrics),
    U(GetDC), U(ReleaseDC), U(ShowCursor), U(SetCursor), U(LoadCursorA), U(LoadIconA), U(LoadBitmapA), U(GetCursorPos), U(ClientToScreen), U(SetCapture), U(ReleaseCapture), U(GetCapture),
    U(GetAsyncKeyState), U(GetKeyState), U(GetDoubleClickTime), U(MessageBoxA), U(SetWindowTextA), U(GetWindowLongA), U(SetWindowLongA), U(InvalidateRect), U(ValidateRect), U(PtInRect), U(wsprintfA),
    U(IsClipboardFormatAvailable), U(OpenClipboard), U(CloseClipboard), U(GetClipboardData), U(SetPropA), U(GetPropA), U(RemovePropA),
    U(DialogBoxParamA), U(DialogBoxIndirectParamA), U(CreateDialogIndirectParamA), U(EndDialog), U(GetDlgItem), U(SetDlgItemTextA), U(IsDlgButtonChecked), U(LoadStringA),
    G(GetDeviceCaps), G(GetDeviceGammaRamp), G(SetDeviceGammaRamp), G(CreateCompatibleDC), G(CreateFontIndirectA), G(DeleteObject), G(SelectObject), G(SetTextColor), G(StretchBlt), G(GetObjectA),
    A(RegOpenKeyExA), A(RegOpenKeyA), A(RegCreateKeyExA), A(RegSetValueExA), A(RegQueryValueExA), A(RegCloseKey), A(CryptAcquireContextA), A(CryptReleaseContext), A(CryptCreateHash), A(CryptHashData), A(CryptGetHashParam), A(CryptDestroyHash),
    A(OpenProcessToken), A(OpenThreadToken), A(DuplicateToken), A(AllocateAndInitializeSid), A(FreeSid), A(GetLengthSid), A(InitializeAcl), A(AddAccessAllowedAce), A(InitializeSecurityDescriptor), A(SetSecurityDescriptorDacl), A(SetSecurityDescriptorGroup), A(SetSecurityDescriptorOwner), A(IsValidSecurityDescriptor), A(AccessCheck),
    O(CoInitialize), O(CoUninitialize), O(CoCreateInstance), O(CLSIDFromString), O(StringFromGUID2),
    { "OLEAUT32.dll", "VariantInit", shim_VariantInit }, { "OLEAUT32.dll", "VariantClear", shim_VariantClear },
    { "WINMM.dll", "timeBeginPeriod", shim_timeBeginPeriod }, { "WINMM.dll", "timeEndPeriod", shim_timeEndPeriod },
    { "VERSION.dll", "GetFileVersionInfoSizeA", shim_GetFileVersionInfoSizeA }, { "VERSION.dll", "GetFileVersionInfoA", shim_GetFileVersionInfoA }, { "VERSION.dll", "VerQueryValueA", shim_VerQueryValueA },
    { "SHELL32.dll", "ShellExecuteA", shim_ShellExecuteA }, { "WININET.dll", "InternetQueryOptionA", shim_InternetQueryOptionA },
    { "DSOUND.dll", "DirectSoundCreate8", shim_DirectSoundCreate8 }, { "DSOUND.dll", "DirectSoundCreate", shim_DirectSoundCreate }, { "DSOUND.dll", "#1", shim_DirectSoundCreate }, { "DSOUND.dll", "#11", shim_DirectSoundCreate8 },
    { "DSOUND.dll", "DirectSoundEnumerateA", shim_DirectSoundEnumerateA }, { "DSOUND.dll", "#2", shim_DirectSoundEnumerateA }, { "DSOUND.dll", "DirectSoundCaptureEnumerateA", shim_DirectSoundCaptureEnumerateA }, { "DSOUND.dll", "#9", shim_DirectSoundCaptureEnumerateA }, { "DSOUND.dll", "GetDeviceID", shim_DirectSoundGetDeviceID }, { "DSOUND.dll", "#14", shim_DirectSoundGetDeviceID },
    HOST_DSOUND_SHIM_ENTRIES,
    { "eula.dll", "EBUEula", shim_EBUEula }, { "keystone.dll", "KeystoneCreate", shim_KeystoneCreate }, { "d3d9.dll", "DisableD3DSpy", shim_DisableD3DSpy }, { "d3d9.dll", "DebugSetMute", shim_DebugSetMute }, { "shfolder.dll", "SHGetFolderPathA", shim_SHGetFolderPathA }, { "NVCPL.dll", "NvCplGetDataInt", shim_NvCplGetDataInt }, { "NVCPL.dll", "NvCplSetDataInt", shim_NvCplSetDataInt },
    { "BINKW32.dll", "_BinkOpen@8", shim_BinkOpen }, { "BINKW32.dll", "_BinkSetSoundSystem@8", shim_BinkSetSoundSystem }, { "BINKW32.dll", "_BinkOpenDirectSound@4", shim_BinkOpenDirectSound },
    { "VORBISFILE.dll", "ov_open_callbacks", shim_ov_open_callbacks }, { "VORBISFILE.dll", "ov_clear", shim_ov_clear }, { "VORBISFILE.dll", "ov_read", shim_ov_read }, { "VORBISFILE.dll", "ov_crosslap", shim_ov_crosslap },
    { NULL, NULL, NULL }
};
