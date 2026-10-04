/* Compare the app's update policy with Halo's actual no-update callback.
 * The translated callback is an optional private local input, never bundled. */
#include "multiplayer_update.h"
#include "engine_hooks.h"
#include <assert.h>
#include <stdlib.h>

uint8_t *engine_flat_base;
#if __has_include("sub_005777D0.c")
#include "engine_flags.h"
#include "engine_functions.h"
#include "engine_registers.h"
#include "sub_005777D0.c"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#undef eflags
#define HAVE_ORIGINAL 1
#endif
void engine_dispatch(EngineCPU *cpu,uint32_t address) {
    (void)cpu;(void)address;assert(!"no-update callback must not call network or updater code");abort();
}
int main(void) {
    enum { SIZE=0x800000, STACK=0x10000, RETURN_PC=0x12345678 };
    engine_flat_base=malloc(SIZE);assert(engine_flat_base);
    memset(engine_flat_base,0x5a,SIZE);
    S32(STACK,RETURN_PC);S32(STACK+4,0);
    EngineCPU cpu={0};cpu.gpr[4]=STACK;cpu.gpr[3]=0x87654321;
    host_multiplayer_update_poll(&cpu);
    assert(engine_hooked(0x00577240));
    assert(cpu.gpr[0]==2 && cpu.pc==RETURN_PC && cpu.gpr[4]==STACK+4);
    assert(cpu.gpr[3]==0x87654321);
    puts("PASS native update policy cdecl return and interception");
#ifdef HAVE_ORIGINAL
    uint8_t *expected=malloc(SIZE);assert(expected);memcpy(expected,engine_flat_base,SIZE);
    memset(engine_flat_base,0x5a,SIZE);S32(STACK,RETURN_PC);S32(STACK+4,0);
    cpu=(EngineCPU){0};cpu.gpr[4]=STACK;cpu.gpr[3]=0x87654321;cpu.pc=0x005777D0;
    sub_005777D0(&cpu);
    assert(cpu.pc==RETURN_PC && cpu.gpr[4]==STACK+4 && cpu.gpr[3]==0x87654321);
    /* The original callback saves EBX below ESP, unlike the native leaf. */
    S32(STACK-4,0x5a5a5a5a);
    assert(!memcmp(expected,engine_flat_base,SIZE));
    free(expected);puts("PASS all guest memory matches the original 1.10 no-update result");
#else
    puts("SKIP original callback comparison: private generated engine not present");
#endif
    free(engine_flat_base);return 0;
}
