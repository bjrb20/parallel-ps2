/* What the drive asks of the emulator around it, from C: defined in the
 * C++ units named. C89. */
#pragma once

#include "../../common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The fast-boot option (libretro/main.cpp). */
bool host_option_fast_boot(void);
/* VMManager::Internal::GameStartingOnCPUThread (VMManager.cpp). */
void vm_game_starting_on_cpu_thread(void);
/* VMManager::SetState(VMState::Stopping) (VMManager.cpp). */
void vm_request_stop(void);

#ifdef __cplusplus
}
#endif
