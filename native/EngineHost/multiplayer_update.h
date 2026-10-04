#ifndef HALO_MULTIPLAYER_UPDATE_H
#define HALO_MULTIPLAYER_UPDATE_H
#include "host.h"

/* Halo PC 1.10's Internet-menu update poll (00577240) normally launches
 * a Windows patch lookup before opening the server list. Its legacy HTTP
 * endpoint returns an error and can leave that dialog waiting indefinitely.
 * This AOT port requires a hash-verified 1.10 executable; a downloaded Windows
 * patch cannot update the native app. App updates come from project releases.
 *
 * Use the original "no update" state: 005777D0's negative-result callback
 * clears these fields and sets 0069FE04=2. 004A4880 accepts that result and
 * continues to the browser. No game version, CD key, authentication or server
 * compatibility checks are altered. The original master-server client runs.
 */
static void host_multiplayer_update_poll(EngineCPU *cpu) {
    S32(0x007228D4u,0);
    S8(0x007228D8u,0);
    S8(0x007229D8u,0);
    S32(0x0069FE04u,2);
    RET_CDECL(2);
}
#endif
