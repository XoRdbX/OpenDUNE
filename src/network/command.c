/** @file src/network/command.c Game command queue and application. */

#include <string.h>
#include <stdio.h>
#include "../../include/types.h"

#include "command.h"
#include "lockstep.h"
#include "network.h"
#include "../unit.h"
#include "../structure.h"
#include "../house.h"
#include "../pool/unit.h"
#include "../pool/structure.h"
#include "../tools.h"
#include "../tile.h"
#include "../object.h"
#include "../timer.h"

/**
 * Queue a local player command for the upcoming scheduled tick.
 * The command is stored in the lockstep outgoing buffer and will be
 * broadcast to all peers at the start of the next lockstep frame.
 */
void NetCmd_QueueLocal(NetCommandType type, uint16 arg0, uint16 arg1, uint16 arg2)
{
	NetCommand cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.type        = (uint8)type;
	cmd.houseID     = (uint8)g_netConfig.humanHouseIDs[g_netConfig.localPlayerIndex];
	cmd.tick        = (uint16)(g_timerGame + LOCKSTEP_DELAY);
	cmd.arg0        = arg0;
	cmd.arg1        = arg1;
	cmd.arg2        = arg2;
	cmd.playerIndex = g_netConfig.localPlayerIndex;

	NetLockstep_AddLocalCommand(&cmd);
}

/**
 * Apply a command that has been confirmed by the lockstep for the current tick.
 * This is where game state is actually mutated in response to player input.
 */
void NetCmd_Apply(NetCommand *cmd)
{
	Unit      *u;
	Structure *s;

	if (cmd == NULL || cmd->type == CMD_NONE) return;

	switch ((NetCommandType)cmd->type) {

		/* ---- Unit commands ---- */

		case CMD_UNIT_ACTION:
			u = Unit_Get_ByIndex(cmd->arg0);
			if (u == NULL || !u->o.flags.s.used) return;
			Unit_SetAction(u, (ActionType)cmd->arg1);
			break;

		case CMD_UNIT_MOVE:
			u = Unit_Get_ByIndex(cmd->arg0);
			if (u == NULL || !u->o.flags.s.used) return;
			/* Clear previous orders same as viewport does */
			Object_Script_Variable4_Clear(&u->o);
			u->targetAttack = 0;
			u->targetMove   = 0;
			u->route[0]     = 0xFF;
			Unit_SetAction(u, ACTION_MOVE);
			Unit_SetDestination(u, cmd->arg1);
			break;

		case CMD_UNIT_ATTACK:
			u = Unit_Get_ByIndex(cmd->arg0);
			if (u == NULL || !u->o.flags.s.used) return;
			Object_Script_Variable4_Clear(&u->o);
			u->targetAttack = 0;
			u->targetMove   = 0;
			u->route[0]     = 0xFF;
			Unit_SetAction(u, ACTION_ATTACK);
			Unit_SetTarget(u, cmd->arg1);
			break;

		case CMD_UNIT_HARVEST:
			u = Unit_Get_ByIndex(cmd->arg0);
			if (u == NULL || !u->o.flags.s.used) return;
			Object_Script_Variable4_Clear(&u->o);
			u->targetAttack = 0;
			u->targetMove   = cmd->arg1;
			u->route[0]     = 0xFF;
			Unit_SetAction(u, ACTION_HARVEST);
			break;

		case CMD_UNIT_DEPLOY:
			u = Unit_Get_ByIndex(cmd->arg0);
			if (u == NULL || !u->o.flags.s.used) return;
			Unit_SetAction(u, ACTION_DEPLOY);
			break;

		/* ---- Structure commands ---- */

		case CMD_STRUCTURE_BUILD:
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			Structure_BuildObject(s, cmd->arg1);
			break;

		case CMD_STRUCTURE_CANCEL:
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			Structure_BuildObject(s, 0xFFFF);
			break;

		case CMD_STRUCTURE_PLACE:
			/* Structure placement is complex; the active structure pointer is
			 * shared state set prior to queuing. We simply re-apply the place
			 * at the encoded tile position. */
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			Structure_Place(s, cmd->arg1);
			break;

		case CMD_STRUCTURE_REPAIR:
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			Structure_SetRepairingState(s, -1, NULL);
			break;

		case CMD_STRUCTURE_UPGRADE:
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			Structure_SetUpgradingState(s, -1, NULL);
			break;

		case CMD_STRUCTURE_HOLD:
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			s->o.flags.s.onHold = !s->o.flags.s.onHold;
			break;

		case CMD_STRUCTURE_LAUNCH:
			s = Structure_Get_ByIndex(cmd->arg0);
			if (s == NULL || !s->o.flags.s.used) return;
			Structure_ActivateSpecial(s);
			break;

		default:
			fprintf(stderr, "[NET] Unknown command type %d\n", cmd->type);
			break;
	}
}
