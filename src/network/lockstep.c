/** @file src/network/lockstep.c Deterministic lockstep synchronisation.
 *
 * Protocol
 * --------
 * Every game tick N, each player:
 *   1. Sends a TICK_COMMANDS packet for tick N+LOCKSTEP_DELAY containing their
 *      queued commands (may be empty).
 *   2. Waits until it has received TICK_COMMANDS from every other player for
 *      the current tick N (busy-polls Net_Recv with a timeout).
 *   3. Applies all received commands for tick N.
 *   4. Runs the normal game loop (GameLoop_Unit, etc.).
 *   5. Optionally exchanges a TICK_CHECKSUM packet for desync detection.
 *
 * Packet wire layout (all little-endian):
 *   [0]    MSG_TICK_COMMANDS (0xD1)
 *   [1]    playerIndex
 *   [2-3]  tick (uint16 LE)
 *   [4]    command count (0..NET_MAX_CMDS_TICK)
 *   [5..]  count * NET_CMD_WIRE_SIZE bytes of NetCommand structs
 *
 *   [0]    MSG_TICK_CHECKSUM (0xD2)
 *   [1]    playerIndex
 *   [2-3]  tick (uint16 LE)
 *   [4-7]  checksum (uint32 LE)
 */

#include <string.h>
#include <stdio.h>
#include "../../include/types.h"

#include "lockstep.h"
#include "network.h"
#include "command.h"
#include "../pool/pool.h"
#include "../pool/unit.h"
#include "../pool/structure.h"
#include "../unit.h"
#include "../structure.h"
#include "../house.h"
#include "../tile.h"
#include "../tools.h"

#define MSG_TICK_COMMANDS  0xD1
#define MSG_TICK_CHECKSUM  0xD2

/* Ring buffer: LOCKSTEP_BUFFER_TICKS slots, indexed by (tick % LOCKSTEP_BUFFER_TICKS). */
static NetTickBuffer s_tickBuf[LOCKSTEP_BUFFER_TICKS];

/* Outgoing queue for local commands (will be sent for tick currentTick+LOCKSTEP_DELAY). */
static NetCommand    s_localQueue[NET_MAX_CMDS_TICK];
static uint8         s_localCount = 0;

/* Checksum exchange */
static uint32 s_checksumSent[LOCKSTEP_BUFFER_TICKS];
static uint32 s_checksumRecv[NET_MAX_PLAYERS][LOCKSTEP_BUFFER_TICKS];
static bool   s_checksumRecvFlag[NET_MAX_PLAYERS][LOCKSTEP_BUFFER_TICKS];

/* Local lockstep tick counter — starts at 0 on NetLockstep_Init regardless of
 * g_timerGame.  Using g_timerGame directly is wrong because both peers start
 * their process-local g_timerGame at different absolute values (host and client
 * don't launch at the same instant), so they would index different ring-buffer
 * slots and never rendezvous. */
static uint32 s_lockstepTick = 0;

static bool s_initialised = false;

void NetLockstep_Init(void)
{
	uint8 i, t;

	memset(s_tickBuf, 0, sizeof(s_tickBuf));
	memset(s_localQueue, 0, sizeof(s_localQueue));
	memset(s_checksumSent, 0, sizeof(s_checksumSent));
	memset(s_checksumRecv, 0, sizeof(s_checksumRecv));
	memset(s_checksumRecvFlag, 0, sizeof(s_checksumRecvFlag));
	s_localCount    = 0;
	s_lockstepTick  = 0;
	s_initialised   = true;

	/* Bootstrap the ring buffer for the first LOCKSTEP_DELAY ticks.
	 *
	 * Protocol: at tick N we send commands for N+DELAY and wait for tick-N
	 * data.  But at tick 0 nobody has ever sent tick-0 commands — the first
	 * send goes to slot DELAY.  Without seeding the buffer both sides sit
	 * waiting for tick-0 data that never arrives and time out.
	 *
	 * Fix: pre-mark the first LOCKSTEP_DELAY slots as "received" with
	 * zero commands for every possible player.  Unconnected players are
	 * skipped by the wait loop's connected check, so marking them here is
	 * harmless.  Slot N % LOCKSTEP_BUFFER_TICKS is cleared and re-populated
	 * in the normal flow once the game has been running long enough. */
	for (t = 0; t < LOCKSTEP_DELAY; t++) {
		uint8 slot = (uint8)(t % LOCKSTEP_BUFFER_TICKS);
		for (i = 0; i < NET_MAX_PLAYERS; i++) {
			s_tickBuf[slot].count[i]    = 0;
			s_tickBuf[slot].received[i] = true;
		}
	}

	fprintf(stderr, "[NET] NetLockstep_Init: bootstrapped %u slot(s) (DELAY=%u BUFFER=%u)\n",
	        LOCKSTEP_DELAY, LOCKSTEP_DELAY, LOCKSTEP_BUFFER_TICKS);

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (!g_netConfig.peers[i].connected) continue;
		fprintf(stderr, "[NET]   peer[%u] = %s:%u%s\n",
		        (unsigned)i, g_netConfig.peers[i].ip, g_netConfig.peers[i].port,
		        (i == g_netConfig.localPlayerIndex) ? " (self)" : "");
	}
}

void NetLockstep_Uninit(void)
{
	s_initialised = false;
}

void NetLockstep_AddLocalCommand(NetCommand *cmd)
{
	if (!s_initialised) return;
	if (s_localCount >= NET_MAX_CMDS_TICK) {
		fprintf(stderr, "[NET] Local command queue full, dropping command %d\n", cmd->type);
		return;
	}
	s_localQueue[s_localCount++] = *cmd;
}

/* ----- Internal packet builders ----- */

static uint16 BuildCommandsPacket(uint8 *buf, uint32 tick,
                                  const NetCommand *cmds, uint8 count)
{
	uint16 off = 0;
	uint8  i;
	buf[off++] = MSG_TICK_COMMANDS;
	buf[off++] = g_netConfig.localPlayerIndex;
	buf[off++] = (uint8)(tick & 0xFF);
	buf[off++] = (uint8)((tick >> 8) & 0xFF);
	buf[off++] = count;
	for (i = 0; i < count; i++) {
		memcpy(buf + off, &cmds[i], NET_CMD_WIRE_SIZE);
		off += NET_CMD_WIRE_SIZE;
	}
	return off;
}

static uint16 BuildChecksumPacket(uint8 *buf, uint32 tick, uint32 checksum)
{
	uint16 off = 0;
	buf[off++] = MSG_TICK_CHECKSUM;
	buf[off++] = g_netConfig.localPlayerIndex;
	buf[off++] = (uint8)(tick & 0xFF);
	buf[off++] = (uint8)((tick >> 8) & 0xFF);
	buf[off++] = (uint8)(checksum & 0xFF);
	buf[off++] = (uint8)((checksum >>  8) & 0xFF);
	buf[off++] = (uint8)((checksum >> 16) & 0xFF);
	buf[off++] = (uint8)((checksum >> 24) & 0xFF);
	return off;
}

/* ----- Send this tick's commands to all peers ----- */

void NetLockstep_FlushSend(uint32 gameTick)
{
	uint8  pkt[NET_MAX_PACKET_SIZE];
	uint16 len;
	uint32 scheduledTick = s_lockstepTick + LOCKSTEP_DELAY;
	uint8  i;

	len = BuildCommandsPacket(pkt, scheduledTick, s_localQueue, s_localCount);

	/* Also store locally so we don't wait for ourselves */
	{
		uint8 slot = (uint8)(scheduledTick % LOCKSTEP_BUFFER_TICKS);
		uint8 li   = g_netConfig.localPlayerIndex;
		uint8 c    = s_localCount;
		uint8 j;

		if (c > NET_MAX_CMDS_TICK) c = NET_MAX_CMDS_TICK;
		for (j = 0; j < c; j++) {
			s_tickBuf[slot].cmds[li][j] = s_localQueue[j];
		}
		s_tickBuf[slot].count[li]    = c;
		s_tickBuf[slot].received[li] = true;
	}

	/* Send to all other players */
	for (i = 0; i < g_netConfig.playerCount; i++) {
		bool sent;
		if (i == g_netConfig.localPlayerIndex) continue;
		if (!g_netConfig.peers[i].connected) continue;
		sent = Net_Send(i, pkt, len);
		fprintf(stderr, "[NET] gameTick=%u lstick=%u FlushSend -> peer[%u] sched=%u cmds=%u ok=%d\n",
		        (unsigned)gameTick, (unsigned)s_lockstepTick,
		        (unsigned)i, (unsigned)scheduledTick,
		        (unsigned)s_localCount, (int)sent);
	}

	/* Clear local queue */
	memset(s_localQueue, 0, sizeof(s_localQueue));
	s_localCount = 0;
}

/* ----- Pump incoming packets into the buffer ----- */

void NetLockstep_PumpReceive(void)
{
	uint8  buf[NET_MAX_PACKET_SIZE];
	uint8  sender;
	int16  len;

	while ((len = Net_Recv(&sender, buf, sizeof(buf))) > 0) {
		if (len < 5) continue;

		if (buf[0] == MSG_TICK_COMMANDS) {
			uint8  pi    = buf[1];
			uint32 tick  = (uint32)buf[2] | ((uint32)buf[3] << 8);
			uint8  count = buf[4];
			uint8  slot  = (uint8)(tick % LOCKSTEP_BUFFER_TICKS);
			uint8  j;

			if (pi >= NET_MAX_PLAYERS) continue;
			if (count > NET_MAX_CMDS_TICK) count = NET_MAX_CMDS_TICK;
			if (len < 5 + count * NET_CMD_WIRE_SIZE) continue;

			for (j = 0; j < count; j++) {
				memcpy(&s_tickBuf[slot].cmds[pi][j],
				       buf + 5 + j * NET_CMD_WIRE_SIZE,
				       NET_CMD_WIRE_SIZE);
			}
			s_tickBuf[slot].count[pi]    = count;
			s_tickBuf[slot].received[pi] = true;

			fprintf(stderr, "[NET] PumpReceive: COMMANDS tick=%u from peer[%u] cmds=%u\n",
			        (unsigned)tick, (unsigned)pi, (unsigned)count);

		} else if (buf[0] == MSG_TICK_CHECKSUM) {
			uint8  pi    = buf[1];
			uint32 tick  = (uint32)buf[2] | ((uint32)buf[3] << 8);
			uint32 csum  = (uint32)buf[4] | ((uint32)buf[5] << 8) |
			               ((uint32)buf[6] << 16) | ((uint32)buf[7] << 24);
			uint8  slot  = (uint8)(tick % LOCKSTEP_BUFFER_TICKS);
			if (pi >= NET_MAX_PLAYERS) continue;
			s_checksumRecv[pi][slot]     = csum;
			s_checksumRecvFlag[pi][slot] = true;
		}
	}
}

/* ----- Desync detection ----- */

uint32 NetLockstep_ComputeChecksum(void)
{
	uint32 crc = 0x12345678;
	PoolFindStruct find;
	Unit      *u;
	Structure *s;

	/* Hash unit positions and hitpoints */
	memset(&find, 0, sizeof(find));
	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	u = Unit_Find(&find);
	while (u != NULL) {
		crc ^= (uint32)u->o.index * 0x9E3779B9;
		crc ^= (uint32)u->o.position.x;
		crc ^= (uint32)u->o.position.y << 16;
		crc ^= (uint32)u->o.hitpoints << 8;
		crc  = (crc << 7) | (crc >> 25); /* rotate */
		u = Unit_Find(&find);
	}

	/* Hash structure states */
	memset(&find, 0, sizeof(find));
	find.houseID = HOUSE_INVALID;
	find.type    = 0xFFFF;
	find.index   = 0xFFFF;

	s = Structure_Find(&find);
	while (s != NULL) {
		crc ^= (uint32)s->o.index * 0x6C62272E;
		crc ^= (uint32)s->o.hitpoints << 4;
		crc ^= (uint32)s->countDown;
		crc  = (crc << 13) | (crc >> 19);
		s = Structure_Find(&find);
	}

	return crc;
}

static void CheckChecksums(uint32 gameTick)
{
	uint8  slot = (uint8)(gameTick % LOCKSTEP_BUFFER_TICKS);
	uint8  i;
	uint32 myCheck = s_checksumSent[slot];

	for (i = 0; i < g_netConfig.playerCount; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (!s_checksumRecvFlag[i][slot]) continue;
		if (s_checksumRecv[i][slot] != myCheck) {
			fprintf(stderr, "[NET] DESYNC at tick %u: local=%08X peer%d=%08X\n",
			        (unsigned)gameTick, (unsigned)myCheck,
			        (int)i, (unsigned)s_checksumRecv[i][slot]);
			/* TODO: disconnect or reconnect logic */
		}
		s_checksumRecvFlag[i][slot] = false;
	}
}

/* ----- Main lockstep tick ----- */

/**
 * Must be called at the start of each game tick, before game logic.
 * Returns true if the tick may proceed, false if we timed out waiting for peers.
 */
bool NetLockstep_Tick(uint32 gameTick)
{
	uint8  slot;
	uint8  i;
	uint32 deadline;
	uint32 checksum;
	uint8  pkt[16];
	uint16 plen;

	if (!s_initialised || !g_netConfig.active) return true;

	/* Use s_lockstepTick, NOT gameTick, for ring-buffer indexing.
	 * g_timerGame starts counting at process launch, so host and client have
	 * different absolute values when the game loop begins.  s_lockstepTick
	 * resets to 0 at NetLockstep_Init() for both peers, keeping them aligned. */
	slot = (uint8)(s_lockstepTick % LOCKSTEP_BUFFER_TICKS);

	fprintf(stderr, "[NET] lstick=%u gameTick=%u slot=%u buf=[",
	        (unsigned)s_lockstepTick, (unsigned)gameTick, (unsigned)slot);
	for (i = 0; i < g_netConfig.playerCount; i++) {
		if (!g_netConfig.peers[i].connected) { fprintf(stderr, "-"); continue; }
		fprintf(stderr, "%c", s_tickBuf[slot].received[i] ? 'R' : '?');
	}
	fprintf(stderr, "]\n");

	/* 1. Send our commands for s_lockstepTick+LOCKSTEP_DELAY */
	NetLockstep_FlushSend(gameTick);

	/* 2. Compute and send checksum for previous tick */
	checksum = NetLockstep_ComputeChecksum();
	s_checksumSent[slot] = checksum;

	plen = BuildChecksumPacket(pkt, s_lockstepTick, checksum);
	for (i = 0; i < g_netConfig.playerCount; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (!g_netConfig.peers[i].connected) continue;
		Net_Send(i, pkt, plen);
	}

	/* 3. Wait until we have commands from all players for s_lockstepTick */
	deadline = Net_GetTime() + LOCKSTEP_TIMEOUT_MS;
	for (;;) {
		bool allReady = true;

		NetLockstep_PumpReceive();

		for (i = 0; i < g_netConfig.playerCount; i++) {
			if (!g_netConfig.peers[i].connected) continue;
			if (!s_tickBuf[slot].received[i]) {
				allReady = false;
				break;
			}
		}

		if (allReady) break;

		if (Net_GetTime() > deadline) {
			fprintf(stderr, "[NET] Timeout lstick=%u gameTick=%u slot=%u. Missing: [",
			        (unsigned)s_lockstepTick, (unsigned)gameTick, (unsigned)slot);
			for (i = 0; i < g_netConfig.playerCount; i++) {
				if (!g_netConfig.peers[i].connected) continue;
				if (!s_tickBuf[slot].received[i]) {
					fprintf(stderr, " peer[%u]=%s:%u", (unsigned)i,
					        g_netConfig.peers[i].ip, g_netConfig.peers[i].port);
				}
			}
			fprintf(stderr, " ]\n");
			return false;
		}
	}

	fprintf(stderr, "[NET] lstick=%u ready, applying %u slots\n",
	        (unsigned)s_lockstepTick, (unsigned)g_netConfig.playerCount);

	/* 4. Apply all commands for this tick */
	for (i = 0; i < g_netConfig.playerCount; i++) {
		uint8 j;
		uint8 cnt = s_tickBuf[slot].count[i];
		for (j = 0; j < cnt; j++) {
			NetCmd_Apply(&s_tickBuf[slot].cmds[i][j]);
		}
	}

	/* 5. Check desync (uses previous tick's checksums) */
	CheckChecksums(s_lockstepTick);

	/* 6. Reset slot for reuse */
	memset(&s_tickBuf[slot], 0, sizeof(s_tickBuf[slot]));

	s_lockstepTick++;
	return true;
}
