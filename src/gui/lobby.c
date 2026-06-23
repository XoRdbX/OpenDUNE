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
 *   LOBBY_MSG_BYE    (0xA6) client → host: graceful disconnect
 *
 * ROSTER wire format (per slot, 21 bytes):
 *   [+0]      filled      (1 byte)
 *   [+1]      houseChoice (1 byte)
 *   [+2]      reserved    (1 byte, was "ready")
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
#define LOBBY_MSG_BYE      0xA6

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
} LobbySlot;

static LobbySlot s_slots[NET_MAX_PLAYERS];

/* ------------------------------------------------------------------ */

/* Non-blocking key read that avoids flooding the input ring buffer.
 * Input_Keyboard_NextKey() calls Input_AddHistory(0) every invocation;
 * at ~1000Hz that would overflow the 512-entry buffer and bury real
 * keystrokes.  Only call it when the buffer already has something. */
static uint16 LobbyPollKey(void)
{
	if (Input_IsInputAvailable() == 0) return 0;
	return Input_Keyboard_NextKey();
}

/* Test whether k is any representation of the Enter key. */
static bool IsEnterKey(uint16 k)
{
	/* 0x0D = CR, 0x2B = NUMPAD5/RETURN (GameLoop), 0x4C = KP Enter, 0x61 = Enter alt */
	return k == 0x0D || k == 0x2B || k == 0x4C || k == 0x61;
}

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
			snprintf(buf, sizeof(buf), "  Slot %d: %s  [%s]",
			         i + 1, s_slots[i].ip,
			         s_houseNames[s_slots[i].houseChoice % 3]);
			col = (i == mySlot) ? 14 : 10;
		}
		GUI_DrawText_Wrapper(buf, 10, 46 + i * 12, col, 0, 0x12);
	}

	if (statusLine != NULL && statusLine[0] != '\0') {
		GUI_DrawText_Wrapper(statusLine, 10, 90, 6, 0, 0x12);
	}

	GUI_DrawText_Wrapper(isHost ? "ENTER = Start  ESC = Cancel"
	                            : "ESC = Cancel",
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
		if (IsEnterKey(key)) {
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
	uint8  pkt[2 + NET_MAX_PLAYERS * ROSTER_SLOT_SIZE];
	uint16 off = 0;
	uint8  i;

	pkt[off++] = LOBBY_MSG_ROSTER;
	pkt[off++] = (uint8)NET_MAX_PLAYERS;

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		pkt[off++] = s_slots[i].filled ? 1 : 0;
		pkt[off++] = s_slots[i].houseChoice;
		pkt[off++] = 0; /* reserved */
		memset(pkt + off, 0, 16);
		{
			size_t iplen = strlen(s_slots[i].ip);
			if (iplen > 15) iplen = 15;
			memcpy(pkt + off, s_slots[i].ip, iplen);
		}
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

/* ---- Slot helpers ---- */

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
			if (isHost && sender < NET_MAX_PLAYERS && len >= 2) {
				s_slots[sender].filled      = true;
				s_slots[sender].houseChoice = buf[1] % 3;
				snprintf(s_slots[sender].ip, sizeof(s_slots[sender].ip),
				         "%s", g_netConfig.peers[sender].ip);
				s_slots[sender].port = g_netConfig.peers[sender].port;
			}
			break;

		case LOBBY_MSG_BYE:
			if (isHost && sender < NET_MAX_PLAYERS) {
				s_slots[sender].filled             = false;
				g_netConfig.peers[sender].connected = false;
			}
			break;

		case LOBBY_MSG_ROSTER:
			if (!isHost && len >= 2) {
				uint8  cnt = buf[1];
				uint8  i;
				uint16 off = 2;
				for (i = 0; i < cnt && i < NET_MAX_PLAYERS; i++) {
					char   ip[17];
					uint16 port;
					if (off + ROSTER_SLOT_SIZE > (uint16)len) break;
					s_slots[i].filled      = (buf[off] != 0);
					s_slots[i].houseChoice = buf[off + 1] % 3;
					memcpy(ip, buf + off + 3, 16);
					ip[16] = '\0';
					memcpy(s_slots[i].ip, ip, 17);
					port = (uint16)buf[off + 19] | ((uint16)buf[off + 20] << 8);
					s_slots[i].port = port;
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
	bool   running  = true;
	bool   started  = false;
	bool   needsRedraw = true;
	uint32 lastRoster  = 0;
	uint32 lastDraw    = 0;
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

		snprintf(g_netConfig.peers[0].ip, sizeof(g_netConfig.peers[0].ip), "127.0.0.1");
		g_netConfig.peers[0].port      = LOBBY_PORT;
		g_netConfig.peers[0].connected = true;

		s_slots[0].filled      = true;
		s_slots[0].houseChoice = myHouseChoice;
		snprintf(s_slots[0].ip, sizeof(s_slots[0].ip), "localhost");
		s_slots[0].port = LOBBY_PORT;

	} else {
		char hostIP[64] = {0};
		if (!PromptIP("Host IP", hostIP, sizeof(hostIP))) return false;

		mySlot = 1;
		g_netConfig.localPlayerIndex = mySlot;

		Net_SetPeerAddr(0, hostIP, LOBBY_PORT);

		s_slots[mySlot].filled      = true;
		s_slots[mySlot].houseChoice = myHouseChoice;
		snprintf(s_slots[mySlot].ip, sizeof(s_slots[mySlot].ip), "me");
	}

	/* ---- Open socket ----
	 * Host binds to the well-known port; client binds to an OS-assigned
	 * ephemeral port so two instances on the same machine don't conflict. */
	if (!Net_Init(isHost ? LOBBY_PORT : 0)) {
		GUI_DrawText_Wrapper("ERROR: Cannot open UDP socket!", 10, 150, 4, 0, 0x12);
		GFX_SetPalette(g_palette1);
		sleepIdle();
		return false;
	}

	snprintf(statusLine, sizeof(statusLine),
	         isHost ? "Waiting for clients... (ENTER to start, need 2+)"
	                : "Connecting to host...");

	/* ---- Lobby loop ---- */
	while (running) {
		uint32 now = Net_GetTime();

		/* Throttle redraws to ~10 fps; redraw immediately on state changes. */
		if (needsRedraw || (now - lastDraw) >= 100) {
			LobbyDraw(isHost, mySlot, statusLine);
			needsRedraw = false;
			lastDraw    = now;
		}

		/* Handle input — gate the key poll behind IsInputAvailable so we do
		 * not call Input_AddHistory(0) thousands of times per second. */
		{
			uint16 k = LobbyPollKey();

			if (k == 0x1B) { /* ESC = cancel */
				if (!isHost) {
					uint8 bye[1] = { LOBBY_MSG_BYE };
					Net_Send(0, bye, 1);
				}
				running = false;
				break;
			}

			if (isHost && IsEnterKey(k)) {
				uint8 i, filledCount = 0;
				for (i = 0; i < NET_MAX_PLAYERS; i++) {
					if (s_slots[i].filled) filledCount++;
				}
				if (filledCount >= 2) {
					uint32 seed = (uint32)(now ^ ((uint32)g_timerGame << 16));
					g_netConfig.sharedSeed = seed;
					LobbyBroadcastStart(seed);
					started = true;
					running = false;
				} else {
					snprintf(statusLine, sizeof(statusLine),
					         "Need at least 2 players to start!");
					needsRedraw = true;
				}
			}
		}

		/* Receive packets */
		if (isHost) {
			uint8  buf[NET_MAX_PACKET_SIZE];
			char   srcIP[64];
			uint16 srcPort;
			int16  rlen;

			while ((rlen = Net_RecvAny(buf, sizeof(buf), srcIP, &srcPort)) > 0) {
				int8 slot = LobbyFindSlot(srcIP, srcPort);

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

			if (now - lastRoster > LOBBY_REFRESH_MS) {
				LobbyBroadcastRoster();
				lastRoster = now;
			}

		} else {
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

	/* ---- Finalise g_netConfig ---- */
	{
		uint8 i;
		for (i = 0; i < NET_MAX_PLAYERS; i++) {
			/* Mark unfilled slots with HOUSE_MAX so Scenario_Load_House won't
			 * accidentally treat them as human players. */
			g_netConfig.humanHouseIDs[i] = s_slots[i].filled
			                               ? s_slots[i].houseChoice
			                               : (uint8)HOUSE_MAX;
		}
		g_netConfig.active      = true;
		g_netConfig.playerCount = NET_MAX_PLAYERS;
	}

	NetLockstep_Init();
	return true;
}
