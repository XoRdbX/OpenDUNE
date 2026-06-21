/** @file src/network/command.h Game command definitions for multiplayer lockstep. */

#ifndef COMMAND_H
#define COMMAND_H

#include "../../include/types.h"
#include "network.h"

/**
 * Types of player commands that must be synchronised across all clients.
 * All commands originate from a human player and are deferred by LOCKSTEP_DELAY ticks.
 */
typedef enum NetCommandType {
	CMD_NONE              = 0,

	/* Unit orders */
	CMD_UNIT_ACTION       = 1,  /*!< Set unit action (move, guard, attack...). args: unitIndex, actionType */
	CMD_UNIT_MOVE         = 2,  /*!< Move unit. args: unitIndex, encodedDest */
	CMD_UNIT_ATTACK       = 3,  /*!< Attack target. args: unitIndex, encodedTarget */
	CMD_UNIT_HARVEST      = 4,  /*!< Harvest at position. args: unitIndex, encodedDest */

	/* Structure orders */
	CMD_STRUCTURE_BUILD   = 5,  /*!< Start building object. args: structIndex, objectType */
	CMD_STRUCTURE_PLACE   = 6,  /*!< Place built structure. args: structIndex, packedPosition */
	CMD_STRUCTURE_REPAIR  = 7,  /*!< Toggle repair. args: structIndex */
	CMD_STRUCTURE_UPGRADE = 8,  /*!< Toggle upgrade. args: structIndex */
	CMD_STRUCTURE_CANCEL  = 9,  /*!< Cancel/next build. args: structIndex */
	CMD_STRUCTURE_HOLD    = 10, /*!< Toggle on-hold. args: structIndex */
	CMD_STRUCTURE_LAUNCH  = 11, /*!< Activate palace special. args: structIndex */

	/* Harvester extra order */
	CMD_UNIT_DEPLOY       = 12, /*!< Deploy MCV. args: unitIndex */

	CMD_MAX
} NetCommandType;

/** Wire format of a single command (16 bytes, little-endian). */
MSVC_PACKED_BEGIN
typedef struct NetCommand {
	uint8  type;         /*!< NetCommandType */
	uint8  houseID;      /*!< Source house (for validation). */
	uint16 tick;         /*!< Game tick at which to apply this command. */
	uint16 arg0;         /*!< First argument (unit/structure index). */
	uint16 arg1;         /*!< Second argument (encoded target, objectType, ...). */
	uint16 arg2;         /*!< Third argument (reserved). */
	uint8  playerIndex;  /*!< Sender player slot index. */
	uint8  _pad[5];      /*!< Padding to 16 bytes. */
} GCC_PACKED NetCommand;
MSVC_PACKED_END

#define NET_CMD_WIRE_SIZE  16
#define NET_MAX_CMDS_TICK  32  /*!< Max commands per tick per player. */
#define LOCKSTEP_DELAY      2  /*!< Ticks ahead we schedule commands. */

/** Per-tick command collection: one slot per player, up to NET_MAX_CMDS_TICK commands. */
typedef struct NetTickBuffer {
	NetCommand cmds[NET_MAX_PLAYERS][NET_MAX_CMDS_TICK];
	uint8      count[NET_MAX_PLAYERS];
	bool       received[NET_MAX_PLAYERS]; /*!< Have we received this player's commands? */
} NetTickBuffer;

extern void NetCmd_QueueLocal(NetCommandType type, uint16 arg0, uint16 arg1, uint16 arg2);
extern void NetCmd_Apply(NetCommand *cmd);

#endif /* COMMAND_H */
