/** @file src/gui/lobby.c Multiplayer lobby — text-based setup screen.
 *
 * This is an intentionally simple terminal-style UI that fits inside the
 * existing 320x200 game screen.  The flow is:
 *
 *   Host path  : player selects "Host", enters own house, waits for clients.
 *   Client path: player selects "Join", enters host IP, selects own house.
 *
 * Once all NET_MAX_PLAYERS slots are filled and the host presses Start, the
 * host broadcasts a START packet carrying the shared random seed and the
 * house assignments, then all machines transition to GM_RESTART.
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

/* Lobby packet types */
#define LOBBY_MSG_HELLO    0xA1  /* client → host: announce presence + preferred house */
#define LOBBY_MSG_ROSTER   0xA2  /* host → all: current player list */
#define LOBBY_MSG_START    0xA3  /* host → all: game start params */
#define LOBBY_MSG_PING     0xA4  /* bidirectional keepalive */

#define LOBBY_PORT         NET_DEFAULT_PORT
#define LOBBY_REFRESH_MS   500   /* roster broadcast interval */

/* ------------------------------------------------------------------ */

static const char *s_houseNames[3] = { "Harkonnen", "Atreides", "Ordos" };

/* Simple 3-player roster used during lobby negotiation */
typedef struct LobbySlot {
	bool   filled;
	char   ip[64];
	uint16 port;
	uint8  houseChoice; /* 0=Harkonnen,1=Atreides,2=Ordos */
	bool   ready;
} LobbySlot;

static LobbySlot s_slots[NET_MAX_PLAYERS];

/* ------------------------------------------------------------------ */

static void LobbyDraw(bool isHost, uint8 mySlot, const char *statusLine)
{
	int i;
	char buf[64];

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

/* Send a simple PING to all known peers. */
static void LobbyPingAll(void)
{
	uint8 pkt[2] = { LOBBY_MSG_PING, g_netConfig.localPlayerIndex };
	uint8 i;
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (g_netConfig.peers[i].connected) Net_Send(i, pkt, 2);
	}
}

/* Broadcast roster from host to all clients. */
static void LobbyBroadcastRoster(void)
{
	uint8 pkt[2 + NET_MAX_PLAYERS * (64 + 4)];
	uint16 off = 0;
	uint8  i;

	pkt[off++] = LOBBY_MSG_ROSTER;
	pkt[off++] = (uint8)NET_MAX_PLAYERS;
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		uint8 filled = s_slots[i].filled ? 1 : 0;
		pkt[off++] = filled;
		pkt[off++] = s_slots[i].houseChoice;
		pkt[off++] = s_slots[i].ready ? 1 : 0;
		memcpy(pkt + off, s_slots[i].ip, 16); /* send first 16 chars */
		off += 16;
	}

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (g_netConfig.peers[i].connected) Net_Send(i, pkt, off);
	}
}

/* Broadcast START to all clients and populate g_netConfig fully. */
static void LobbyBroadcastStart(uint32 seed)
{
	uint8 pkt[16];
	uint8 i;
	uint16 off = 0;

	pkt[off++] = LOBBY_MSG_START;
	pkt[off++] = (uint8)NET_MAX_PLAYERS;
	/* seed (4 bytes LE) */
	pkt[off++] = (uint8)(seed & 0xFF);
	pkt[off++] = (uint8)((seed >>  8) & 0xFF);
	pkt[off++] = (uint8)((seed >> 16) & 0xFF);
	pkt[off++] = (uint8)((seed >> 24) & 0xFF);
	/* house IDs */
	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		pkt[off++] = s_slots[i].houseChoice;
	}

	for (i = 0; i < NET_MAX_PLAYERS; i++) {
		if (i == g_netConfig.localPlayerIndex) continue;
		if (g_netConfig.peers[i].connected) Net_Send(i, pkt, off);
	}
}

/* Parse a received lobby packet. */
static void LobbyHandlePacket(uint8 sender, uint8 *buf, int16 len, bool isHost)
{
	if (len < 1) return;

	switch (buf[0]) {
		case LOBBY_MSG_PING:
			/* Client announced itself — fill slot on host side */
			if (isHost && sender < NET_MAX_PLAYERS) {
				if (!s_slots[sender].filled) {
					s_slots[sender].filled = true;
					snprintf(s_slots[sender].ip, sizeof(s_slots[sender].ip),
					         "%s", g_netConfig.peers[sender].ip);
					s_slots[sender].houseChoice = sender; /* default */
				}
			}
			break;

		case LOBBY_MSG_HELLO:
			/* buf[1] = houseChoice */
			if (isHost && sender < NET_MAX_PLAYERS && len >= 2) {
				s_slots[sender].filled = true;
				s_slots[sender].houseChoice = buf[1] % 3;
				snprintf(s_slots[sender].ip, sizeof(s_slots[sender].ip),
				         "%s", g_netConfig.peers[sender].ip);
			}
			break;

		case LOBBY_MSG_ROSTER:
			/* Client processes roster update from host */
			if (!isHost && len >= 2) {
				uint8 cnt = buf[1];
				uint8 i;
				uint16 off = 2;
				for (i = 0; i < cnt && i < NET_MAX_PLAYERS; i++) {
					if (off + 19 > (uint16)len) break;
					s_slots[i].filled      = (buf[off] != 0);
					s_slots[i].houseChoice = buf[off + 1] % 3;
					s_slots[i].ready       = (buf[off + 2] != 0);
					memcpy(s_slots[i].ip, buf + off + 3, 16);
					s_slots[i].ip[15] = '\0';
					off += 19;
				}
			}
			break;

		case LOBBY_MSG_START:
			/* Client receives start signal */
			if (!isHost && len >= 6) {
				uint32 seed;
				uint8 i;
				seed  = (uint32)buf[2];
				seed |= (uint32)buf[3] << 8;
				seed |= (uint32)buf[4] << 16;
				seed |= (uint32)buf[5] << 24;
				g_netConfig.sharedSeed = seed;
				for (i = 0; i < NET_MAX_PLAYERS && (6 + i) < len; i++) {
					g_netConfig.humanHouseIDs[i] = buf[6 + i] % 3;
				}
			}
			break;

		default:
			break;
	}
}

/* Prompt the user to type an IP address using the keyboard.
 * Returns true if confirmed, false if cancelled. */
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

		if (key == 0x1B) return false; /* ESC */
		if (key == 0x0D || key == 0x4C) { /* Enter */
			if (pos > 0) {
				snprintf(ipOut, maxLen, "%s", buf);
				return true;
			}
			continue;
		}
		if ((key == 0x08 || key == 0x7F) && pos > 0) {
			buf[--pos] = '\0';
			continue;
		}
		if (pos < maxLen - 1 && key >= 0x20 && key <= 0x7E) {
			buf[pos++] = (char)key;
			buf[pos]   = '\0';
		}
	}
}

/* Main lobby entry point */
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
	uint8  buf[NET_MAX_PACKET_SIZE];
	uint8  sender;
	int16  rlen;

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
	g_netConfig.active       = true;
	g_netConfig.isHost       = isHost;
	g_netConfig.playerCount  = NET_MAX_PLAYERS;

	if (isHost) {
		mySlot = 0;
		g_netConfig.localPlayerIndex = 0;

		/* Collect peer IPs */
		{
			uint8 i;
			for (i = 1; i < NET_MAX_PLAYERS; i++) {
				char ipbuf[64] = {0};
				char label[32];
				snprintf(label, sizeof(label), "IP of player %d", i + 1);
				if (!PromptIP(label, ipbuf, sizeof(ipbuf))) return false;
				snprintf(g_netConfig.peers[i].ip, sizeof(g_netConfig.peers[i].ip), "%s", ipbuf);
				g_netConfig.peers[i].port      = LOBBY_PORT;
				g_netConfig.peers[i].connected = true;
			}
		}

		/* Self peer entry */
		snprintf(g_netConfig.peers[0].ip, 16, "127.0.0.1");
		g_netConfig.peers[0].port      = LOBBY_PORT;
		g_netConfig.peers[0].connected = true;

		/* Fill our own slot */
		s_slots[0].filled      = true;
		s_slots[0].houseChoice = myHouseChoice;
		s_slots[0].ready       = false;
		snprintf(s_slots[0].ip, sizeof(s_slots[0].ip), "localhost");

	} else {
		/* Join: ask for host IP */
		char hostIP[64] = {0};
		if (!PromptIP("Host IP", hostIP, sizeof(hostIP))) return false;

		/* Our slot index is assigned by the host; use 1 or 2 by default.
		 * A proper implementation would negotiate; here we pick the first free. */
		mySlot = 1; /* Will be corrected by roster */
		g_netConfig.localPlayerIndex = mySlot;

		/* Host is peer 0 */
		snprintf(g_netConfig.peers[0].ip, sizeof(g_netConfig.peers[0].ip), "%s", hostIP);
		g_netConfig.peers[0].port      = LOBBY_PORT;
		g_netConfig.peers[0].connected = true;

		/* Self */
		snprintf(g_netConfig.peers[mySlot].ip, 16, "127.0.0.1");
		g_netConfig.peers[mySlot].port      = LOBBY_PORT;
		g_netConfig.peers[mySlot].connected = true;

		s_slots[mySlot].filled      = true;
		s_slots[mySlot].houseChoice = myHouseChoice;
		snprintf(s_slots[mySlot].ip, sizeof(s_slots[mySlot].ip), "client");
	}

	/* ---- Open socket ---- */
	if (!Net_Init(LOBBY_PORT)) {
		GUI_DrawText_Wrapper("ERROR: Cannot open UDP socket!", 10, 150, 4, 0, 0x12);
		GFX_SetPalette(g_palette1);
		sleepIdle();
		return false;
	}

	snprintf(statusLine, sizeof(statusLine),
	         isHost ? "Waiting for clients to connect..." : "Connecting to host...");

	/* ---- Lobby loop ---- */
	while (running) {
		uint32 now = Net_GetTime();

		/* Draw */
		LobbyDraw(isHost, mySlot, statusLine);

		/* Handle input */
		{
			uint16 k = Input_Keyboard_NextKey();
			if (k == 0x1B) { running = false; break; }

			if (isHost && (k == 0x0D || k == 0x4C)) {
				/* Host pressed Enter: check all slots filled and start */
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
				}
			}

			if (!isHost && k == ' ') {
				/* Client toggles ready */
				uint8 hello[2] = { LOBBY_MSG_HELLO, myHouseChoice };
				s_slots[mySlot].ready = !s_slots[mySlot].ready;
				Net_Send(0, hello, 2);
			}
		}

		/* Receive packets */
		while ((rlen = Net_Recv(&sender, buf, sizeof(buf))) > 0) {
			LobbyHandlePacket(sender, buf, rlen, isHost);
			if (!isHost && buf[0] == LOBBY_MSG_START) {
				started = true;
				running = false;
				break;
			}
		}

		/* Host: periodically broadcast roster */
		if (isHost && now - lastRoster > LOBBY_REFRESH_MS) {
			LobbyBroadcastRoster();
			LobbyPingAll();
			lastRoster = now;
		}

		/* Client: periodically ping host */
		if (!isHost && now - lastRoster > LOBBY_REFRESH_MS) {
			uint8 hello[2] = { LOBBY_MSG_HELLO, myHouseChoice };
			Net_Send(0, hello, 2);
			lastRoster = now;
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
