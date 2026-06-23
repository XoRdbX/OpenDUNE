/** @file src/gui/lobby.c Multiplayer lobby — text-based setup screen.
 *
 * Connection model
 * ----------------
 *   Host  : binds 0.0.0.0:port, waits for clients to HELLO.  No need to
 *            know client IPs in advance — they are learned dynamically.
 *   Client: enters only the host IP.  The host assigns the client's slot
 *            index and sends the full roster (including other clients' IPs)
 *            so every peer can talk directly to every other peer.
 *
 * Lobby packet types
 * ------------------
 *   LOBBY_MSG_HELLO  (0xA1) client → host: announce + preferred house
 *   LOBBY_MSG_ROSTER (0xA2) host → all:   current player list (IPs + ports)
 *   LOBBY_MSG_START  (0xA3) host → all:   game start params
 *   LOBBY_MSG_PING   (0xA4) bidirectional keepalive
 *   LOBBY_MSG_SLOT   (0xA5) host → client: "you are slot N"
 *
 * ROSTER wire format (per slot, 21 bytes):
 *   [+0]      filled      (1 byte)
 *   [+1]      houseChoice (1 byte)
 *   [+2]      ready       (1 byte)
 *   [+3..+18] ip          (16 bytes, null-padded, dotted-decimal)
 *   [+19..+20] port       (2 bytes LE)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../include/types.h"

#include "lobby.h"
#include "gui.h"
#include "font.h"
#include "../network/network.h"
#include "../network/lockstep.h"
#include "../network/command.h"
#include "../input/input.h"
#include "../house.h"
#include "../tools.h"
#include "../timer.h"
#include "../gfx.h"
#include "../string.h"
#include "../os/sleep.h"

#define LOBBY_MSG_HELLO    0xA1
#define LOBBY_MSG_ROSTER   0xA2
#define LOBBY_MSG_START    0xA3
#define LOBBY_MSG_PING     0xA4
#define LOBBY_MSG_SLOT     0xA5

#define LOBBY_PORT         NET_DEFAULT_PORT
#define LOBBY_REFRESH_MS   500

#define ROSTER_SLOT_SIZE   21   /* bytes per slot in ROSTER packet */

/* ------------------------------------------------------------------ */

static const char *s_houseNames[3] = { "Harkonnen", "Atreides", "Ordos" };

typedef struct LobbySlot {
	bool   filled;
	char   ip[64];
	uint16 port;
	uint8  houseChoice;
	bool   ready;
} LobbySlot;

static LobbySlot s_slots[NET_MAX_PLAYERS];

/* ------------------------------------------------------------------ */

static void LobbyDraw(bool isHost, uint8 mySlot, const char *statusLine)
{
	int i;
	char buf[80];

	GFX_Screen_SetActive(SCREEN_0);
	GUI_DrawFilledRectangle(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, 0);

	Font_Select(g_fontNew8p);
	GUI_DrawText_Wrapper("== OpenDUNE Multiplayer ==", 10, 5, 15, 0, 0x12);
	GUI_DrawText_Wrapper(isHost ? "Role: HOST" : "Role: CLIENT", 10, 18, 11, 0, 0x12);

	GUI_DrawText_Wrapper("Players:", 10, 35, 15, 0, 0x12);
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		uint8 col;
		if (!s_slots[i].filled) {
			snprintf(buf, sizeof(buf), "  Slot %d: (waiting...)", i + 1);
			col = 7;
		} else {
			snprintf(buf, sizeof(buf), "  Slot %d: %s  [%s]%s",
			         i + 1, s_slots[i].ip,
			         s_houseNames[s_slots[i].houseChoice],
			         s_slots[i].ready ? " READY" : "");
			col = (i == mySlot) ? 14 : 10;
		}
		GUI_DrawText_Wrapper(buf, 10, 46 + i * 12, col, 0, 0x12);
	}

	if (statusLine != NULL) {
		GUI_DrawText_Wrapper(statusLine, 10, 90, 6, 0, 0x12);
	}

	GUI_DrawText_Wrapper(isHost ? "ENTER=Start game  ESC=Cancel"
	                            : "SPACE=Toggle Ready  ESC=Cancel",
	                     10, 180, 7, 0, 0x12);

	GFX_SetPalette(g_palette1);
}

/* Prompt the user to type an IP address using the keyboard. */
static bool PromptIP(const char *label, char *ipOut, uint16 maxLen)
{
	uint16 pos = 0;
	char   buf[64];
	char   display[80];

	memset(buf, 0, sizeof(buf));

	for (;;) {
		uint16 key;

		snprintf(display, sizeof(display), "%s: %s_", label, buf);
		GUI_DrawFilledRectangle(0, 110, SCREEN_WIDTH - 1, 130, 0);
		GUI_DrawText_Wrapper(display, 10, 112, 15, 0, 0x12);
		GFX_SetPalette(g_palette1);

		key = Input_WaitForValidInput();

		if (key == 0x1B) return false;
		if (key == 0x0D || key == 0x4C) {
			if (pos > 0) { snprintf(ipOut, maxLen, "%s", buf); return true; }
			continue;
		}
		if ((key == 0x08 || key == 0x7F) && pos > 0) { buf[--pos] = '\0'; continue; }
		if (pos < maxLen - 1 && key >= 0x20 && key <= 0x7E) {
			buf[pos++] = (char)key;
			buf[pos]   = '\0';
		}
	}
}

/* ---- Packet builders ---- */

static void LobbyBroadcastRoster(void)
{
	/* 2-byte header + ROSTER_SLOT_SIZE bytes per slot */
	uint8  pkt[2 + NET_MAX_PLAYERS * ROSTER_SLOT_SIZE];
	uint16 off = 0;
	uint8  i;

	pkt[off++] = LOBBY_MSG_ROSTER;
	pkt[off++] = (uint8)NET_MAX_PLAYERS;

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		pkt[off++] = s_slots[i].filled ? 1 : 0;
		pkt[off++] = s_slots[i].houseChoice;
		pkt[off++] = s_slots[i].ready ? 1 : 0;
		memset(pkt + off, 0, 16);
		memcpy(pkt + off, s_slots[i].ip,
		       strlen(s_slots[i].ip) < 16 ? strlen(s_slots[i].ip) : 15);
		off += 16;
		pkt[off++] = (uint8)(s_slots[i].port & 0xFF);
		pkt[off++] = (uint8)((s_slots[i].port >> 8) & 0xFF);
	}

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (g_netConfig.peers[i].connected) Net_Send(i, pkt, off);
	}
}

static void LobbyBroadcastStart(uint32 seed)
{
	uint8  pkt[16];
	uint16 off = 0;
	uint8  i;

	pkt[off++] = LOBBY_MSG_START;
	pkt[off++] = (uint8)NET_MAX_PLAYERS;
	pkt[off++] = (uint8)(seed & 0xFF);
	pkt[off++] = (uint8)((seed >>  8) & 0xFF);
	pkt[off++] = (uint8)((seed >> 16) & 0xFF);
	pkt[off++] = (uint8)((seed >> 24) & 0xFF);
	for (i = 0; i < NET_MAX_PLAYERS; i++) pkt[off++] = s_slots[i].houseChoice;

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (g_netConfig.peers[i].connected) Net_Send(i, pkt, off);
	}
}

/* ---- Find which slot an IP:port belongs to (-1 = unknown). ---- */
static int8 LobbyFindSlot(const char *ip, uint16 port)
{
	uint8 i;
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (!g_netConfig.peers[i].connected) continue;
		if (strcmp(g_netConfig.peers[i].ip, ip) == 0 &&
		    g_netConfig.peers[i].port == port) return (int8)i;
	}
	return -1;
}

/* ---- Find next free (unconnected, non-self) slot on host. ---- */
static int8 LobbyNextFreeSlot(void)
{
	uint8 i;
	for (i = 1; i < NET_MAX_PLAYERS; i++) {
		if (!g_netConfig.peers[i].connected) return (int8)i;
	}
	return -1;
}

/* ---- Process a received lobby packet ---- */
static void LobbyHandlePacket(uint8 sender, const uint8 *buf, int16 len, bool isHost)
{
	if (len < 1) return;

	switch (buf[0]) {
		case LOBBY_MSG_PING:
			break;

		case LOBBY_MSG_HELLO:
			/* buf[1] = houseChoice */
			if (isHost && sender < NET_MAX_PLAYERS && len >= 2) {
				s_slots[sender].filled      = true;
				s_slots[sender].houseChoice = buf[1] % 3;
				s_slots[sender].ready       = false;
				snprintf(s_slots[sender].ip, sizeof(s_slots[sender].ip),
				         "%s", g_netConfig.peers[sender].ip);
				s_slots[sender].port = g_netConfig.peers[sender].port;
			}
			break;

		case LOBBY_MSG_ROSTER:
			/* Client processes roster update from host */
			if (!isHost && len >= 2) {
				uint8  cnt = buf[1];
				uint8  i;
				uint16 off = 2;
				for (i = 0; i < cnt && i < NET_MAX_PLAYERS; i++) {
					uint16 port;
					char   ip[17];
					if (off + ROSTER_SLOT_SIZE > (uint16)len) break;
					s_slots[i].filled      = (buf[off] != 0);
					s_slots[i].houseChoice = buf[off + 1] % 3;
					s_slots[i].ready       = (buf[off + 2] != 0);
					memcpy(ip, buf + off + 3, 16);
					ip[16] = '\0';
					memcpy(s_slots[i].ip, ip, 17);
					port = (uint16)buf[off + 19] | ((uint16)buf[off + 20] << 8);
					s_slots[i].port = port;

					/* Register non-self, non-host peers we learn about */
					if (s_slots[i].filled && i != g_netConfig.localPlayerIndex
					    && i != 0 && ip[0] != '\0') {
						if (LobbyFindSlot(ip, port) < 0) {
							Net_SetPeerAddr(i, ip, port);
						}
					}
					off += ROSTER_SLOT_SIZE;
				}
			}
			break;

		case LOBBY_MSG_SLOT:
			/* Host tells this client its assigned slot index */
			if (!isHost && len >= 2) {
				uint8 slot = buf[1];
				if (slot > 0 && slot < NET_MAX_PLAYERS) {
					g_netConfig.localPlayerIndex = slot;
				}
			}
			break;

		case LOBBY_MSG_START:
			if (!isHost && len >= 6 + NET_MAX_PLAYERS) {
				uint8  i;
				uint32 seed;
				seed  = (uint32)buf[2];
				seed |= (uint32)buf[3] << 8;
				seed |= (uint32)buf[4] << 16;
				seed |= (uint32)buf[5] << 24;
				g_netConfig.sharedSeed = seed;
				for (i = 0; i < NET_MAX_PLAYERS; i++) {
					g_netConfig.humanHouseIDs[i] = buf[6 + i] % 3;
				}
			}
			break;

		default:
			break;
	}
}

/* ------------------------------------------------------------------ */

bool GUI_Lobby_Show(void)
{
	bool   isHost;
	bool   running = true;
	bool   started = false;
	uint32 lastRoster = 0;
	uint8  mySlot;
	uint8  myHouseChoice = 0;
	uint16 key;
	char   statusLine[80];

	bool   needsRedraw = true;
	uint32 lastDraw    = 0;

	memset(&g_netConfig, 0, sizeof(g_netConfig));
	memset(s_slots, 0, sizeof(s_slots));
	statusLine[0] = '\0';

	/* ---- Ask: Host or Join? ---- */
	GFX_Screen_SetActive(SCREEN_0);
	GUI_DrawFilledRectangle(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, 0);
	Font_Select(g_fontNew8p);
	GUI_DrawText_Wrapper("== OpenDUNE Multiplayer ==", 10, 30, 15, 0, 0x12);
	GUI_DrawText_Wrapper("H = Host a game", 10, 60, 10, 0, 0x12);
	GUI_DrawText_Wrapper("J = Join a game", 10, 75, 10, 0, 0x12);
	GUI_DrawText_Wrapper("ESC = Return to menu",  10, 90, 7, 0, 0x12);
	GFX_SetPalette(g_palette1);

	for (;;) {
		key = Input_WaitForValidInput();
		if (key == 'H' || key == 'h') { isHost = true;  break; }
		if (key == 'J' || key == 'j') { isHost = false; break; }
		if (key == 0x1B) return false;
	}

	/* ---- Choose own house ---- */
	GFX_Screen_SetActive(SCREEN_0);
	GUI_DrawFilledRectangle(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, 0);
	GUI_DrawText_Wrapper("Select your house:", 10, 40, 15, 0, 0x12);
	GUI_DrawText_Wrapper("1 = Harkonnen", 10, 60, 10, 0, 0x12);
	GUI_DrawText_Wrapper("2 = Atreides",  10, 75, 10, 0, 0x12);
	GUI_DrawText_Wrapper("3 = Ordos",     10, 90, 10, 0, 0x12);
	GFX_SetPalette(g_palette1);

	for (;;) {
		key = Input_WaitForValidInput();
		if (key == '1') { myHouseChoice = HOUSE_HARKONNEN; break; }
		if (key == '2') { myHouseChoice = HOUSE_ATREIDES;  break; }
		if (key == '3') { myHouseChoice = HOUSE_ORDOS;     break; }
		if (key == 0x1B) return false;
	}

	/* ---- Configure network ---- */
	g_netConfig.active      = true;
	g_netConfig.isHost      = isHost;
	g_netConfig.playerCount = NET_MAX_PLAYERS;

	if (isHost) {
		mySlot = 0;
		g_netConfig.localPlayerIndex = 0;

		/* Host only knows its own slot; clients will connect dynamically. */
		snprintf(g_netConfig.peers[0].ip, sizeof(g_netConfig.peers[0].ip), "127.0.0.1");
		g_netConfig.peers[0].port      = LOBBY_PORT;
		g_netConfig.peers[0].connected = true;

		s_slots[0].filled      = true;
		s_slots[0].houseChoice = myHouseChoice;
		s_slots[0].ready       = false;
		snprintf(s_slots[0].ip, sizeof(s_slots[0].ip), "localhost");
		s_slots[0].port = LOBBY_PORT;

	} else {
		/* Client: only needs host IP. */
		char hostIP[64] = {0};
		if (!PromptIP("Host IP", hostIP, sizeof(hostIP))) return false;

		mySlot = 1; /* will be updated via LOBBY_MSG_SLOT from host */
		g_netConfig.localPlayerIndex = mySlot;

		/* Register host as peer 0. */
		Net_SetPeerAddr(0, hostIP, LOBBY_PORT);

		s_slots[mySlot].filled      = true;
		s_slots[mySlot].houseChoice = myHouseChoice;
		snprintf(s_slots[mySlot].ip, sizeof(s_slots[mySlot].ip), "me");
	}

	/* ---- Open socket ----
	 * Host binds to the well-known port so clients can reach it.
	 * Client binds to port 0 (OS picks a free ephemeral port) so that
	 * multiple players on the same machine don't fight over the same port.
	 * The host learns the client's actual port from the UDP source address. */
	if (!Net_Init(isHost ? LOBBY_PORT : 0)) {
		GUI_DrawText_Wrapper("ERROR: Cannot open UDP socket!", 10, 150, 4, 0, 0x12);
		GFX_SetPalette(g_palette1);
		sleepIdle();
		return false;
	}

	snprintf(statusLine, sizeof(statusLine),
	         isHost ? "Waiting for clients... (ENTER to start)"
	                : "Connecting to host...");

	/* ---- Lobby loop ---- */
	while (running) {
		uint32 now = Net_GetTime();

		if (needsRedraw || (now - lastDraw) >= 100) {
			LobbyDraw(isHost, mySlot, statusLine);
			needsRedraw = false;
			lastDraw    = now;
		}

		/* Handle input */
		{
			uint16 k = Input_Keyboard_NextKey();
			if (k == 0x1B) { running = false; break; }

			if (isHost && (k == 0x0D || k == 0x4C)) {
				bool allFilled = true;
				uint8 i;
				for (i = 0; i < NET_MAX_PLAYERS; i++) {
					if (!s_slots[i].filled) { allFilled = false; break; }
				}
				if (allFilled) {
					uint32 seed = (uint32)(now ^ ((uint32)g_timerGame << 16));
					g_netConfig.sharedSeed = seed;
					LobbyBroadcastStart(seed);
					started = true;
					running = false;
				} else {
					snprintf(statusLine, sizeof(statusLine),
					         "Not all players connected yet!");
					needsRedraw = true;
				}
			}

			if (!isHost && k == ' ') {
				uint8 hello[2] = { LOBBY_MSG_HELLO, myHouseChoice };
				s_slots[mySlot].ready = !s_slots[mySlot].ready;
				Net_Send(0, hello, 2);
			}
		}

		/* Receive packets */
		if (isHost) {
			/* Accept packets from any source; dynamically register new clients. */
			uint8  buf[NET_MAX_PACKET_SIZE];
			char   srcIP[64];
			uint16 srcPort;
			int16  rlen;

			while ((rlen = Net_RecvAny(buf, sizeof(buf), srcIP, &srcPort)) > 0) {
				int8 slot = LobbyFindSlot(srcIP, srcPort);

				/* New sender sending HELLO → assign a slot. */
				if (slot < 0 && rlen >= 1 && buf[0] == LOBBY_MSG_HELLO) {
					slot = LobbyNextFreeSlot();
					if (slot >= 0) {
						uint8 slotPkt[2];
						Net_SetPeerAddr((uint8)slot, srcIP, srcPort);
						slotPkt[0] = LOBBY_MSG_SLOT;
						slotPkt[1] = (uint8)slot;
						Net_Send((uint8)slot, slotPkt, 2);
					}
				}

				if (slot >= 0) {
					LobbyHandlePacket((uint8)slot, buf, rlen, true);
					needsRedraw = true;
					if (buf[0] == LOBBY_MSG_START) {
						started = true;
						running = false;
						break;
					}
				}
			}

			/* Periodically broadcast roster and ping. */
			if (now - lastRoster > LOBBY_REFRESH_MS) {
				LobbyBroadcastRoster();
				lastRoster = now;
			}

		} else {
			/* Client: receive via registered peers (host + any others). */
			uint8  buf[NET_MAX_PACKET_SIZE];
			uint8  sender;
			int16  rlen;

			while ((rlen = Net_Recv(&sender, buf, sizeof(buf))) > 0) {
				if (buf[0] == LOBBY_MSG_SLOT) {
					uint8 assigned = buf[1];
					if (assigned > 0 && assigned < NET_MAX_PLAYERS) {
						mySlot = assigned;
						g_netConfig.localPlayerIndex = mySlot;
						s_slots[mySlot].filled      = true;
						s_slots[mySlot].houseChoice = myHouseChoice;
						snprintf(s_slots[mySlot].ip, sizeof(s_slots[mySlot].ip), "me");
					}
				}
				LobbyHandlePacket(sender, buf, rlen, false);
				needsRedraw = true;
				if (buf[0] == LOBBY_MSG_START) {
					started = true;
					running = false;
					break;
				}
			}

			/* Periodically ping host with HELLO (carries house choice). */
			if (now - lastRoster > LOBBY_REFRESH_MS) {
				uint8 hello[2] = { LOBBY_MSG_HELLO, myHouseChoice };
				Net_Send(0, hello, 2);
				lastRoster = now;
			}
		}

		sleepIdle();
	}

	if (!started) {
		Net_Uninit();
		memset(&g_netConfig, 0, sizeof(g_netConfig));
		return false;
	}

	/* ---- Finalise g_netConfig from slot assignments ---- */
	{
		uint8 i;
		for (i = 0; i < NET_MAX_PLAYERS; i++) {
			g_netConfig.humanHouseIDs[i] = s_slots[i].houseChoice;
		}
		g_netConfig.active      = true;
		g_netConfig.playerCount = NET_MAX_PLAYERS;
	}

	NetLockstep_Init();
	return true;
}
