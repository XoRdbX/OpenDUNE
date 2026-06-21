/** @file src/network/lockstep.h Deterministic lockstep synchronisation for multiplayer. */

#ifndef LOCKSTEP_H
#define LOCKSTEP_H

#include "../../include/types.h"
#include "command.h"

/** Maximum ticks we buffer ahead (must be >= LOCKSTEP_DELAY). */
#define LOCKSTEP_BUFFER_TICKS  8
#define LOCKSTEP_TIMEOUT_MS  2000  /*!< ms to wait for lagging peers before giving up. */

extern void NetLockstep_Init(void);
extern void NetLockstep_Uninit(void);

/* Called once per game tick, before game logic runs. */
extern bool NetLockstep_Tick(uint32 gameTick);

/* Queue a local command for the scheduled future tick. */
extern void NetLockstep_AddLocalCommand(NetCommand *cmd);

/* Internal: packet receive / send loop called by Tick(). */
extern void NetLockstep_FlushSend(uint32 gameTick);
extern void NetLockstep_PumpReceive(void);

/* Checksum for desync detection (computed each tick). */
extern uint32 NetLockstep_ComputeChecksum(void);

#endif /* LOCKSTEP_H */
